/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsGeoBlurSettings.h"

////////////////////////////////////////////////////
// nsGeoPositionCoords
////////////////////////////////////////////////////
nsGeoBlurSettings::nsGeoBlurSettings()
  : mBlurType(GEO_BLUR_TYPE_PRECISE)
  , mRadius(0)
  , mHasValidCoords(false)
  , mLatitude(0)
  , mLongitude(0)
{
}

int32_t
nsGeoBlurSettings::GetRadius()
{
  return mRadius;
}

double
nsGeoBlurSettings::GetLatitude()
{
  return mLatitude;
}

double
nsGeoBlurSettings::GetLongitude()
{
  return mLongitude;
}

nsString
nsGeoBlurSettings::GetManifestURL()
{
	return mManifestURL;
}

void
nsGeoBlurSettings::SetManifestURL(nsString aManifestURL)
{
	mManifestURL = aManifestURL;
}

void
nsGeoBlurSettings::SetRadius(int32_t aRadius)
{
	mRadius = aRadius;
}

void
nsGeoBlurSettings::SetBlurType(int32_t aBlurType)
{
	mBlurType = aBlurType;
}

void
nsGeoBlurSettings::SetCoords(nsString aCoords)
{
	ClearCoords();

	if(aCoords.IsEmpty()) {
		return;
	}

	uint32_t middle = aCoords.Find(",");
	if(aCoords.CharAt(0) == '@' && middle > 0 && middle < aCoords.Length() - 1) {
		nsString val = aCoords;
		val.Cut(middle, aCoords.Length() - middle);
		val.Cut(0, 1);
		nsresult result;
		mLatitude = val.ToDouble(&result);

		if(result != NS_OK) {
			mLatitude = 0;
			return;
		}

		val = aCoords;
		val.Cut(0, middle + 1);
		mLongitude = val.ToDouble(&result);

		if(result != NS_OK) {
			mLatitude = 0;
			mLongitude = 0;
			return;
		}

		mHasValidCoords = true;
	}
}

void
nsGeoBlurSettings::ClearCoords() {
  mHasValidCoords = false;

	mLatitude = 0;
	mLongitude = 0;
}

bool
nsGeoBlurSettings::IsExactLoaction()
{
	return mBlurType == GEO_BLUR_TYPE_PRECISE;
}

bool
nsGeoBlurSettings::IsFakeLocation()
{
	return mBlurType == GEO_BLUR_TYPE_CUSTOM;
}

bool
nsGeoBlurSettings::IsBluredLocation()
{
	return mBlurType == GEO_BLUR_TYPE_BLUR;
}

bool
nsGeoBlurSettings::IsNoLocation()
{
	return mBlurType == GEO_BLUR_TYPE_NO_LOCATION;
}

bool
nsGeoBlurSettings::HasValidCoords()
{
	return mHasValidCoords;
}
