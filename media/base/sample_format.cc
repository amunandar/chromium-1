// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/base/sample_format.h"

#include "base/logging.h"

namespace media {

int SampleFormatToBytesPerChannel(SampleFormat sample_format) {
  switch (sample_format) {
    case kUnknownSampleFormat:
      return 0;
    case kSampleFormatU8:
      return 1;
    case kSampleFormatS16:
    case kSampleFormatPlanarS16:
      return 2;
    case kSampleFormatS32:
    case kSampleFormatF32:
    case kSampleFormatPlanarF32:
      return 4;
    case kSampleFormatMax:
      break;
  }

  NOTREACHED() << "Invalid sample format provided: " << sample_format;
  return 0;
}

}  // namespace media
