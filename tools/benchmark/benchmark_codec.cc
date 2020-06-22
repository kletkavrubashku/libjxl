// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "tools/benchmark/benchmark_codec.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "jxl/base/data_parallel.h"
#include "jxl/base/os_specific.h"
#include "jxl/base/padded_bytes.h"
#include "jxl/base/profiler.h"
#include "jxl/base/span.h"
#include "jxl/base/status.h"
#include "jxl/codec_in_out.h"
#include "jxl/color_encoding.h"
#include "jxl/color_management.h"
#include "jxl/image.h"
#include "jxl/image_bundle.h"
#include "jxl/image_ops.h"
#include "tools/benchmark/benchmark_args.h"
#include "tools/benchmark/benchmark_codec_custom.h"
#ifdef BENCHMARK_JPEG
#include "tools/benchmark/benchmark_codec_jpeg.h"
#endif  // BENCHMARK_JPEG
#include "tools/benchmark/benchmark_codec_jxl.h"
#include "tools/benchmark/benchmark_codec_png.h"
#include "tools/benchmark/benchmark_stats.h"

#ifdef BENCHMARK_WEBP
#include "tools/benchmark/benchmark_codec_webp.h"
#endif  // BENCHMARK_WEBP

namespace jxl {

void ImageCodec::ParseParameters(const std::string& parameters) {
  params_ = parameters;
  std::vector<std::string> parts = SplitString(parameters, ':');
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!ParseParam(parts[i])) {
      JXL_ABORT("Invalid parameter %s", parts[i].c_str());
    }
  }
}

Status ImageCodec::ParseParam(const std::string& param) {
  if (param[0] ==
      'q') {  // libjpeg-style quality, [0,100]  (or in case of
              // modular, below 0 is also allowed if you like cubism)
    const std::string quality_param = param.substr(1);
    char* end;
    const float q_target = strtof(quality_param.c_str(), &end);
    if (end == quality_param.c_str() ||
        end != quality_param.c_str() + quality_param.size()) {
      return false;
    }
    q_target_ = q_target;
    return true;
  }
  if (param[0] == 'd') {  // butteraugli distance
    const std::string distance_param = param.substr(1);
    char* end;
    const float butteraugli_target = strtof(distance_param.c_str(), &end);
    if (end == distance_param.c_str() ||
        end != distance_param.c_str() + distance_param.size()) {
      return false;
    }
    butteraugli_target_ = butteraugli_target;

    // full hf asymmetry at high distance
    static const double kHighDistance = 2.5;

    // no hf asymmetry at low distance
    static const double kLowDistance = 0.6;

    if (butteraugli_target_ >= kHighDistance) {
      hf_asymmetry_ = args_.hf_asymmetry;
    } else if (butteraugli_target_ >= kLowDistance) {
      float w =
          (butteraugli_target_ - kLowDistance) / (kHighDistance - kLowDistance);
      hf_asymmetry_ = args_.hf_asymmetry * w + 1.0f * (1.0f - w);
    } else {
      hf_asymmetry_ = 1.0f;
    }
    return true;
  } else if (param[0] == 'r') {
    butteraugli_target_ = -1.0;
    hf_asymmetry_ = args_.hf_asymmetry;
    bitrate_target_ = strtof(param.substr(1).c_str(), nullptr);
    return true;
  }
  return false;
}

// Low-overhead "codec" for measuring benchmark overhead.
class NoneCodec : public ImageCodec {
 public:
  explicit NoneCodec(const BenchmarkArgs& args) : ImageCodec(args) {}
  Status ParseParam(const std::string& param) override { return true; }

  Status Compress(const std::string& filename, const CodecInOut* io,
                  ThreadPool* pool, PaddedBytes* compressed,
                  jpegxl::tools::SpeedStats* speed_stats) override {
    PROFILER_ZONE("NoneCompress");
    const double start = Now();
    // Encode image size so we "decompress" something of the same size, as
    // required by butteraugli.
    const uint32_t xsize = io->xsize();
    const uint32_t ysize = io->ysize();
    compressed->resize(8);
    memcpy(compressed->data(), &xsize, 4);
    memcpy(compressed->data() + 4, &ysize, 4);
    const double end = Now();
    speed_stats->NotifyElapsed(end - start);
    return true;
  }

  Status Decompress(const std::string& filename,
                    const Span<const uint8_t> compressed, ThreadPool* pool,
                    CodecInOut* io,
                    jpegxl::tools::SpeedStats* speed_stats) override {
    PROFILER_ZONE("NoneDecompress");
    const double start = Now();
    JXL_ASSERT(compressed.size() == 8);
    uint32_t xsize, ysize;
    memcpy(&xsize, compressed.data(), 4);
    memcpy(&ysize, compressed.data() + 4, 4);
    Image3F image(xsize, ysize);
    ZeroFillImage(&image);
    io->metadata.SetFloat32Samples();
    io->metadata.color_encoding = ColorEncoding::SRGB();
    io->SetFromImage(std::move(image), io->metadata.color_encoding);
    const double end = Now();
    speed_stats->NotifyElapsed(end - start);
    return true;
  }

  void GetMoreStats(BenchmarkStats* stats) override {}
};

ImageCodecPtr CreateImageCodec(const std::string& description) {
  std::string name = description;
  std::string parameters = "";
  size_t colon = description.find(':');
  if (colon < description.size()) {
    name = description.substr(0, colon);
    parameters = description.substr(colon + 1);
  }
  ImageCodecPtr result;
  if (name == "jxl") {
    result.reset(CreateNewJxlCodec(*Args()));
  } else if (name == "custom") {
    result.reset(CreateNewCustomCodec(*Args()));
#ifdef BENCHMARK_JPEG
  } else if (name == "jpeg") {
    result.reset(CreateNewJPEGCodec(*Args()));
#endif  // BENCHMARK_JPEG
  } else if (name == "png") {
    result.reset(CreateNewPNGCodec(*Args()));
  } else if (name == "none") {
    result.reset(new NoneCodec(*Args()));
#ifdef BENCHMARK_WEBP
  } else if (name == "webp") {
    result.reset(CreateNewWebPCodec(*Args()));
#endif  // BENCHMARK_WEBP
  } else {
    JXL_ABORT("Unknown image codec: %s", name.c_str());
  }
  result->set_description(description);
  if (!parameters.empty()) result->ParseParameters(parameters);
  return result;
}

}  // namespace jxl
