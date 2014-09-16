/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsGeoBlurSetting_h
#define nsGeoBlurSetting_h

#include "nsString.h"
#include "mozilla/Attributes.h"

////////////////////////////////////////////////////
// nsGeoBlurSetting
////////////////////////////////////////////////////

#define GEO_BLUR_TYPE_PRECISE    1
#define GEO_BLUR_TYPE_BLUR       2
#define GEO_BLUR_TYPE_CUSTOM     3

/**
 * Simple object that holds a single settings for location.
 */
class nsGeoBlurSettings MOZ_FINAL
{
  public:
    nsGeoBlurSettings();

    bool IsExactLoaction();
    bool IsFakeLocation();
    bool IsBluredLocation();
    bool HasValidCoords();

    int32_t GetRadius();
    double GetLatitude();
    double GetLongitude();
    nsString GetManifestURL();

    void SetManifestURL(nsString aManifestURL);
    void SetBlurType(int32_t aBlurType);
    void SetRadius(int32_t aRadius);
    void SetCoords(nsString aCoords);
    void ClearCoords();

  private:
    nsString mManifestURL;
    int32_t mBlurType;
    int32_t mRadius;
    bool mHasValidCoords;
    double mLatitude, mLongitude;
};

#endif /* nsGeoBlurSetting_h */


