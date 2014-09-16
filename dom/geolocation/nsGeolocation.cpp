/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsXULAppAPI.h"

#include "mozilla/dom/ContentChild.h"
#include "mozilla/Telemetry.h"

#include "nsISettingsService.h"

#include "nsGeolocation.h"
#include "nsGeoBlurSettings.h"
#include "nsDOMClassInfoID.h"
#include "nsComponentManagerUtils.h"
#include "nsServiceManagerUtils.h"
#include "nsContentUtils.h"
#include "nsContentPermissionHelper.h"
#include "nsIDocument.h"
#include "nsIObserverService.h"
#include "nsPIDOMWindow.h"
#include "nsThreadUtils.h"
#include "mozilla/Services.h"
#include "mozilla/unused.h"
#include "mozilla/Preferences.h"
#include "mozilla/ClearOnShutdown.h"
#include "mozilla/dom/PermissionMessageUtils.h"

#include "nsJSUtils.h"
#include "prdtoa.h"

class nsIPrincipal;

#ifdef MOZ_ENABLE_QT5GEOPOSITION
#include "QTMLocationProvider.h"
#endif

#ifdef MOZ_WIDGET_ANDROID
#include "AndroidLocationProvider.h"
#endif

#ifdef MOZ_WIDGET_GONK
#include "GonkGPSGeolocationProvider.h"
#endif

#ifdef MOZ_WIDGET_COCOA
#include "CoreLocationLocationProvider.h"
#endif

// Some limit to the number of get or watch geolocation requests
// that a window can make.
#define MAX_GEO_REQUESTS_PER_WINDOW  1500

// The settings key.
#define GEO_SETINGS_ENABLED          "geolocation.enabled"
#define GEO_BLUR_TYPE                "geolocation.blur.type"
#define GEO_BLUR_RADIUS              "geolocation.blur.radius"
#define GEO_BLUR_COORDS              "geolocation.blur.coords"
#define GEO_EXCEPTIONS               "geolocation.exceptions"
#define GEO_UNBLURED                 "geolocation.unblured"

static nsGeoBlurSettings sGlobalBlurSettings;
static nsTArray<nsGeoBlurSettings> sExceptionsAppBlurSettings;
static nsTArray<nsString> sUnbluredApps;

using mozilla::unused;          // <snicker>
using namespace mozilla;
using namespace mozilla::dom;

class nsGeolocationRequest MOZ_FINAL
 : public nsIContentPermissionRequest
 , public nsITimerCallback
 , public nsIGeolocationUpdate
{
 public:
  NS_DECL_CYCLE_COLLECTING_ISUPPORTS
  NS_DECL_NSICONTENTPERMISSIONREQUEST
  NS_DECL_NSITIMERCALLBACK
  NS_DECL_NSIGEOLOCATIONUPDATE

  NS_DECL_CYCLE_COLLECTION_CLASS_AMBIGUOUS(nsGeolocationRequest, nsIContentPermissionRequest)

  nsGeolocationRequest(Geolocation* aLocator,
                       const GeoPositionCallback& aCallback,
                       const GeoPositionErrorCallback& aErrorCallback,
                       PositionOptions* aOptions,
                       bool aWatchPositionRequest = false,
                       int32_t aWatchId = 0);
  void Shutdown();

  void SendLocation(nsIDOMGeoPosition* location);
  bool WantsHighAccuracy() {return !mShutdown && mOptions && mOptions->mEnableHighAccuracy;}
  void SetTimeoutTimer();
  void StopTimeoutTimer();
  void NotifyErrorAndShutdown(uint16_t);
  nsIPrincipal* GetPrincipal();

  bool IsWatch() { return mIsWatchPositionRequest; }
  int32_t WatchId() { return mWatchId; }
 private:
  virtual ~nsGeolocationRequest();

  double GridAlgorithm(int32_t aRadius, double aKmSize, double aCoord);
  double CalcLatByGridAlgorithm(int32_t aRadius, double aLatitude);
  double CalcLonByGridAlgorithm(int32_t aRadius, double aLongitude, double aLatitude);
  nsIDOMGeoPosition* FakeLocation(nsIDOMGeoPosition* aPosition, double aLatitude, double aLongitude);
  nsIDOMGeoPosition* BlurLocation(nsIDOMGeoPosition* aPosition, int32_t aRadius);
  nsIDOMGeoPosition* ChangeLocation(nsIDOMGeoPosition* aPosition);

  nsGeoBlurSettings* GetBlurSettings();
  NS_IMETHODIMP GetAppManifestURL(nsString& aAppManifestURL);

  bool mIsWatchPositionRequest;

  nsCOMPtr<nsITimer> mTimeoutTimer;
  GeoPositionCallback mCallback;
  GeoPositionErrorCallback mErrorCallback;
  nsAutoPtr<PositionOptions> mOptions;

  nsRefPtr<Geolocation> mLocator;

  int32_t mWatchId;
  bool mShutdown;
};

static PositionOptions*
CreatePositionOptionsCopy(const PositionOptions& aOptions)
{
  nsAutoPtr<PositionOptions> geoOptions(new PositionOptions());

  geoOptions->mEnableHighAccuracy = aOptions.mEnableHighAccuracy;
  geoOptions->mMaximumAge = aOptions.mMaximumAge;
  geoOptions->mTimeout = aOptions.mTimeout;

  return geoOptions.forget();
}

class GeolocationSettingsCallback : public nsISettingsServiceCallback
{
  virtual ~GeolocationSettingsCallback() {
    MOZ_COUNT_DTOR(GeolocationSettingsCallback);
  }

public:
  NS_DECL_ISUPPORTS

  GeolocationSettingsCallback() {
    MOZ_COUNT_CTOR(GeolocationSettingsCallback);
  }

  NS_IMETHOD Handle(const nsAString& aName, JS::Handle<JS::Value> aResult)
  {
    MOZ_ASSERT(NS_IsMainThread());

    if(aName.EqualsASCII(GEO_SETINGS_ENABLED)) {
      // The geolocation is enabled by default:
      bool value = true;
      if (aResult.isBoolean()) {
        value = aResult.toBoolean();
      }

      MozSettingValue(value);
    } else if(aName.EqualsASCII(GEO_BLUR_TYPE)) {
      int32_t value = GEO_BLUR_TYPE_PRECISE;

      if (aResult.isString()) {
        JSString* jsStr = aResult.toString();
        if (jsStr) {
          nsString str;
          AutoSafeJSContext cx;
          AssignJSString(cx, str, jsStr);
          if(str.EqualsASCII(GEO_BLUR_TYPE_PRECISE_S)) {
            value = GEO_BLUR_TYPE_PRECISE;
          } else if(str.EqualsASCII(GEO_BLUR_TYPE_BLUR_S)) {
            value = GEO_BLUR_TYPE_BLUR;
          } else if(str.EqualsASCII(GEO_BLUR_TYPE_CUSTOM_S)) {
            value = GEO_BLUR_TYPE_CUSTOM;
          } else if(str.EqualsASCII(GEO_BLUR_TYPE_NO_LOCATION_S)) {
            value = GEO_BLUR_TYPE_NO_LOCATION;
          }
        }
      }

      MozSettingBlurTypeValue(value);
    } else if(aName.EqualsASCII(GEO_BLUR_RADIUS)) {
      int32_t value = 0;
      if (aResult.isInt32()) {
        value = aResult.toInt32();
      }

      MozSettingRadiusValue(value);
    } else if(aName.EqualsASCII(GEO_BLUR_COORDS)) {
      JSString* value = nullptr;
      if (aResult.isString()) {
        value = aResult.toString();
      }

      MozSettingCoordsValue(value);
    } else if(aName.EqualsASCII(GEO_EXCEPTIONS)) {
      JSObject* value = nullptr;
      if (aResult.isObject()) {
        value = &aResult.toObject();
      }

      MozSettingExceptionsAppsValue(value);
    } else if(aName.EqualsASCII(GEO_UNBLURED)) {
      JSObject* value = nullptr;
      if (aResult.isObject()) {
        value = &aResult.toObject();
      }

      MozSettingUnbluredAppsValue(value);
    }

    return NS_OK;
  }

  NS_IMETHOD HandleError(const nsAString& aName)
  {
    if(aName.EqualsASCII(GEO_SETINGS_ENABLED)) {
      NS_WARNING("Unable to get value for '" GEO_SETINGS_ENABLED "'");

      // Default it's enabled:
      MozSettingValue(true);
    } else if(aName.EqualsASCII(GEO_BLUR_TYPE)) {
      NS_WARNING("Unable to get value for '" GEO_BLUR_TYPE "'");

      MozSettingBlurTypeValue(GEO_BLUR_TYPE_PRECISE);
    } else if(aName.EqualsASCII(GEO_BLUR_RADIUS)) {
      NS_WARNING("Unable to get value for '" GEO_BLUR_RADIUS "'");

      MozSettingRadiusValue(0);
    } else if(aName.EqualsASCII(GEO_BLUR_COORDS)) {
      NS_WARNING("Unable to get value for '" GEO_BLUR_COORDS "'");

      MozSettingCoordsValue(nullptr);
    } else if(aName.EqualsASCII(GEO_EXCEPTIONS)) {
      NS_WARNING("Unable to get value for '" GEO_EXCEPTIONS "'");

      MozSettingExceptionsAppsValue(nullptr);
    } else if(aName.EqualsASCII(GEO_UNBLURED)) {
      NS_WARNING("Unable to get value for '" GEO_UNBLURED "'");

      MozSettingUnbluredAppsValue(nullptr);
    }

    return NS_OK;
  }

  void MozSettingValue(const bool aValue)
  {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->HandleMozsettingValue(aValue);
    }
  }

  void MozSettingBlurTypeValue(int32_t aValue)
  {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->HandleMozsettingBlurTypeValue(aValue);
    }
  }

  void MozSettingRadiusValue(int32_t aValue)
  {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->HandleMozsettingRadiusValue(aValue);
    }
  }

  void MozSettingCoordsValue(JSString* aValue)
  {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->HandleMozsettingCoordsValue(aValue);
    }
  }

  void MozSettingExceptionsAppsValue(JSObject* aValue)
  {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->HandleMozsettingExceptionsAppsValue(aValue);
    }
  }

  void MozSettingUnbluredAppsValue(JSObject* aValue)
  {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->HandleMozsettingUnbluredAppsValue(aValue);
    }
  }
};

NS_IMPL_ISUPPORTS(GeolocationSettingsCallback, nsISettingsServiceCallback)

class RequestPromptEvent : public nsRunnable
{
public:
  RequestPromptEvent(nsGeolocationRequest* aRequest, nsWeakPtr aWindow)
    : mRequest(aRequest)
    , mWindow(aWindow)
  {
  }

  NS_IMETHOD Run()
  {
    nsCOMPtr<nsPIDOMWindow> window = do_QueryReferent(mWindow);
    nsContentPermissionUtils::AskPermission(mRequest, window);
    return NS_OK;
  }

private:
  nsRefPtr<nsGeolocationRequest> mRequest;
  nsWeakPtr mWindow;
};

class RequestAllowEvent : public nsRunnable
{
public:
  RequestAllowEvent(int allow, nsGeolocationRequest* request)
    : mAllow(allow),
      mRequest(request)
  {
  }

  NS_IMETHOD Run() {
    if (mAllow) {
      mRequest->Allow(JS::UndefinedHandleValue);
    } else {
      mRequest->Cancel();
    }
    return NS_OK;
  }

private:
  bool mAllow;
  nsRefPtr<nsGeolocationRequest> mRequest;
};

class RequestSendLocationEvent : public nsRunnable
{
public:
  RequestSendLocationEvent(nsIDOMGeoPosition* aPosition,
                           nsGeolocationRequest* aRequest)
    : mPosition(aPosition),
      mRequest(aRequest)
  {
  }

  NS_IMETHOD Run() {
    mRequest->SendLocation(mPosition);
    return NS_OK;
  }

private:
  nsCOMPtr<nsIDOMGeoPosition> mPosition;
  nsRefPtr<nsGeolocationRequest> mRequest;
  nsRefPtr<Geolocation> mLocator;
};

////////////////////////////////////////////////////
// PositionError
////////////////////////////////////////////////////

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(PositionError)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIDOMGeoPositionError)
  NS_INTERFACE_MAP_ENTRY(nsIDOMGeoPositionError)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(PositionError, mParent)
NS_IMPL_CYCLE_COLLECTING_ADDREF(PositionError)
NS_IMPL_CYCLE_COLLECTING_RELEASE(PositionError)

PositionError::PositionError(Geolocation* aParent, int16_t aCode)
  : mCode(aCode)
  , mParent(aParent)
{
  SetIsDOMBinding();
}

PositionError::~PositionError(){}


NS_IMETHODIMP
PositionError::GetCode(int16_t *aCode)
{
  NS_ENSURE_ARG_POINTER(aCode);
  *aCode = Code();
  return NS_OK;
}

NS_IMETHODIMP
PositionError::GetMessage(nsAString& aMessage)
{
  switch (mCode)
  {
    case nsIDOMGeoPositionError::PERMISSION_DENIED:
      aMessage = NS_LITERAL_STRING("User denied geolocation prompt");
      break;
    case nsIDOMGeoPositionError::POSITION_UNAVAILABLE:
      aMessage = NS_LITERAL_STRING("Unknown error acquiring position");
      break;
    case nsIDOMGeoPositionError::TIMEOUT:
      aMessage = NS_LITERAL_STRING("Position acquisition timed out");
      break;
    default:
      break;
  }
  return NS_OK;
}

Geolocation*
PositionError::GetParentObject() const
{
  return mParent;
}

JSObject*
PositionError::WrapObject(JSContext* aCx)
{
  return PositionErrorBinding::Wrap(aCx, this);
}

void
PositionError::NotifyCallback(const GeoPositionErrorCallback& aCallback)
{
  nsAutoMicroTask mt;
  if (aCallback.HasWebIDLCallback()) {
    PositionErrorCallback* callback = aCallback.GetWebIDLCallback();

    if (callback) {
      ErrorResult err;
      callback->Call(*this, err);
    }
  } else {
    nsIDOMGeoPositionErrorCallback* callback = aCallback.GetXPCOMCallback();
    if (callback) {
      callback->HandleEvent(this);
    }
  }
}
////////////////////////////////////////////////////
// nsGeolocationRequest
////////////////////////////////////////////////////

nsGeolocationRequest::nsGeolocationRequest(Geolocation* aLocator,
                                           const GeoPositionCallback& aCallback,
                                           const GeoPositionErrorCallback& aErrorCallback,
                                           PositionOptions* aOptions,
                                           bool aWatchPositionRequest,
                                           int32_t aWatchId)
  : mIsWatchPositionRequest(aWatchPositionRequest),
    mCallback(aCallback),
    mErrorCallback(aErrorCallback),
    mOptions(aOptions),
    mLocator(aLocator),
    mWatchId(aWatchId),
    mShutdown(false)
{
}

nsGeolocationRequest::~nsGeolocationRequest()
{
}

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(nsGeolocationRequest)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIContentPermissionRequest)
  NS_INTERFACE_MAP_ENTRY(nsIContentPermissionRequest)
  NS_INTERFACE_MAP_ENTRY(nsITimerCallback)
  NS_INTERFACE_MAP_ENTRY(nsIGeolocationUpdate)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(nsGeolocationRequest)
NS_IMPL_CYCLE_COLLECTING_RELEASE(nsGeolocationRequest)

NS_IMPL_CYCLE_COLLECTION(nsGeolocationRequest, mCallback, mErrorCallback, mLocator)

NS_IMETHODIMP
nsGeolocationRequest::Notify(nsITimer* aTimer)
{
  StopTimeoutTimer();
  NotifyErrorAndShutdown(nsIDOMGeoPositionError::TIMEOUT);
  return NS_OK;
}

void
nsGeolocationRequest::NotifyErrorAndShutdown(uint16_t aErrorCode)
{
  MOZ_ASSERT(!mShutdown, "timeout after shutdown");

  if (!mIsWatchPositionRequest) {
    Shutdown();
    mLocator->RemoveRequest(this);
  }

  NotifyError(aErrorCode);
}

NS_IMETHODIMP
nsGeolocationRequest::GetPrincipal(nsIPrincipal * *aRequestingPrincipal)
{
  NS_ENSURE_ARG_POINTER(aRequestingPrincipal);

  nsCOMPtr<nsIPrincipal> principal = mLocator->GetPrincipal();
  principal.forget(aRequestingPrincipal);

  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationRequest::GetTypes(nsIArray** aTypes)
{
  nsTArray<nsString> emptyOptions;
  return nsContentPermissionUtils::CreatePermissionArray(NS_LITERAL_CSTRING("geolocation"),
                                                         NS_LITERAL_CSTRING("unused"),
                                                         emptyOptions,
                                                         aTypes);
}

NS_IMETHODIMP
nsGeolocationRequest::GetWindow(nsIDOMWindow * *aRequestingWindow)
{
  NS_ENSURE_ARG_POINTER(aRequestingWindow);

  nsCOMPtr<nsIDOMWindow> window = do_QueryReferent(mLocator->GetOwner());
  window.forget(aRequestingWindow);

  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationRequest::GetElement(nsIDOMElement * *aRequestingElement)
{
  NS_ENSURE_ARG_POINTER(aRequestingElement);
  *aRequestingElement = nullptr;
  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationRequest::Cancel()
{
  NotifyError(nsIDOMGeoPositionError::PERMISSION_DENIED);
  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationRequest::Allow(JS::HandleValue aChoices)
{
  MOZ_ASSERT(aChoices.isUndefined());

  // Kick off the geo device, if it isn't already running
  nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
  nsresult rv = gs->StartDevice(GetPrincipal());

  if (NS_FAILED(rv)) {
    // Location provider error
    NotifyError(nsIDOMGeoPositionError::POSITION_UNAVAILABLE);
    return NS_OK;
  }

  bool canUseCache = false;
  CachedPositionAndAccuracy lastPosition = gs->GetCachedPosition();
  if (lastPosition.position) {
    DOMTimeStamp cachedPositionTime_ms;
    lastPosition.position->GetTimestamp(&cachedPositionTime_ms);
    // check to see if we can use a cached value
    // if the user has specified a maximumAge, return a cached value.
    if (mOptions && mOptions->mMaximumAge > 0) {
      uint32_t maximumAge_ms = mOptions->mMaximumAge;
      bool isCachedWithinRequestedAccuracy = WantsHighAccuracy() <= lastPosition.isHighAccuracy;
      bool isCachedWithinRequestedTime =
        DOMTimeStamp(PR_Now() / PR_USEC_PER_MSEC - maximumAge_ms) <= cachedPositionTime_ms;
      canUseCache = isCachedWithinRequestedAccuracy && isCachedWithinRequestedTime;
    }
  }

  gs->UpdateAccuracy(WantsHighAccuracy());
  if (canUseCache) {
    // okay, we can return a cached position
    // getCurrentPosition requests serviced by the cache
    // will now be owned by the RequestSendLocationEvent
    Update(lastPosition.position);
  }

  if (mIsWatchPositionRequest || !canUseCache) {
    // let the locator know we're pending
    // we will now be owned by the locator
    mLocator->NotifyAllowedRequest(this);
  }

  SetTimeoutTimer();

  return NS_OK;
}

void
nsGeolocationRequest::SetTimeoutTimer()
{
  StopTimeoutTimer();

  int32_t timeout;
  if (mOptions && (timeout = mOptions->mTimeout) != 0) {

    if (timeout < 0) {
      timeout = 0;
    } else if (timeout < 10) {
      timeout = 10;
    }

    mTimeoutTimer = do_CreateInstance("@mozilla.org/timer;1");
    mTimeoutTimer->InitWithCallback(this, timeout, nsITimer::TYPE_ONE_SHOT);
  }
}

void
nsGeolocationRequest::StopTimeoutTimer()
{
  if (mTimeoutTimer) {
    mTimeoutTimer->Cancel();
    mTimeoutTimer = nullptr;
  }
}

nsIDOMGeoPosition*
nsGeolocationRequest::ChangeLocation(nsIDOMGeoPosition* aPosition) {
  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    return aPosition;
  }

  nsGeoBlurSettings* blurSettings = GetBlurSettings();

  if(!blurSettings || blurSettings->IsExactLoaction()) {
    return aPosition;
  }

  if(blurSettings->IsBluredLocation()) {
    return BlurLocation(aPosition, blurSettings->GetRadius());
  }

  if(blurSettings->IsFakeLocation() && blurSettings->HasValidCoords()) {
    return FakeLocation(aPosition, blurSettings->GetLatitude(), blurSettings->GetLongitude());
  }

  return nullptr;
}

nsGeoBlurSettings*
nsGeolocationRequest::GetBlurSettings() {
  nsGeoBlurSettings* blurSettings = &sGlobalBlurSettings;
  nsString appManifestURL;

  if(NS_SUCCEEDED(GetAppManifestURL(appManifestURL))) {
    for(size_t i = 0; i < sExceptionsAppBlurSettings.Length(); i++) {
      nsGeoBlurSettings appSettings = sExceptionsAppBlurSettings.ElementAt(i);

      if(appManifestURL == appSettings.GetManifestURL()) {
        blurSettings = &appSettings;
        break;
      }
    }
  }

  return blurSettings;
}

NS_IMETHODIMP
nsGeolocationRequest::GetAppManifestURL(nsString& aAppManifestURL)
{
  aAppManifestURL = mLocator->GetManifestURL();
  if (aAppManifestURL.IsEmpty()) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsIDOMGeoPosition*
nsGeolocationRequest::BlurLocation(nsIDOMGeoPosition* aPosition, int32_t aRadius) {
  if (aPosition) {
    nsCOMPtr<nsIDOMGeoPositionCoords> coords;
    aPosition->GetCoords(getter_AddRefs(coords));

    if (coords) {
      double latitude = 0;
      double longitude = 0;
      coords->GetLatitude(&latitude);
      coords->GetLongitude(&longitude);

      longitude = CalcLonByGridAlgorithm(aRadius, longitude, latitude);
      latitude = CalcLatByGridAlgorithm(aRadius, latitude);

      aPosition = FakeLocation(aPosition, latitude, longitude);
    }
  }

  return aPosition;
}

double
nsGeolocationRequest::GridAlgorithm(int32_t aRadius, double aKmSize, double aCoord)
{
  double gridSize = (aKmSize * aRadius) / 3600;
  double belongsTo = aCoord / gridSize;
  return (floor(belongsTo) * gridSize + ceil(belongsTo) * gridSize) / 2;
}

double
nsGeolocationRequest::CalcLatByGridAlgorithm(int32_t aRadius, double aLatitude)
{
  double kmSize = 32.39;
  return GridAlgorithm(aRadius, kmSize, aLatitude);
}

double
nsGeolocationRequest::CalcLonByGridAlgorithm(int32_t aRadius, double aLongitude, double aLatitude)
{
  double fi = (aLatitude * 3.14) / 180;
  double kmSize = 3600 / (cos(fi) * 111.27);
  return GridAlgorithm(aRadius, kmSize, aLongitude);
}

nsIDOMGeoPosition*
nsGeolocationRequest::FakeLocation(nsIDOMGeoPosition* aPosition, double aLatitude, double aLongitude)
{
  if (!aPosition) {
    return nullptr;
  }

  nsCOMPtr<nsIDOMGeoPositionCoords> coords;
  aPosition->GetCoords(getter_AddRefs(coords));

  if (!coords) {
    return aPosition;
  }

  double altitude = 0;
  double accuracy = 0;
  double altitudeAccuracy = 0;
  double heading = 0;
  double speed = 0;
  DOMTimeStamp timeStamp;

  coords->GetAltitude(&altitude);
  coords->GetAccuracy(&accuracy);
  coords->GetAltitudeAccuracy(&altitudeAccuracy);
  coords->GetHeading(&heading);
  coords->GetSpeed(&speed);

  aPosition->GetTimestamp(&timeStamp);

  return new nsGeoPosition(aLatitude, aLongitude, altitude, accuracy, altitudeAccuracy, heading, speed, timeStamp);
}

void
nsGeolocationRequest::SendLocation(nsIDOMGeoPosition* aPosition)
{
  if (mShutdown) {
    // Ignore SendLocationEvents issued before we were cleared.
    return;
  }

  if (mOptions && mOptions->mMaximumAge > 0) {
    DOMTimeStamp positionTime_ms;
    aPosition->GetTimestamp(&positionTime_ms);
    const uint32_t maximumAge_ms = mOptions->mMaximumAge;
    const bool isTooOld =
        DOMTimeStamp(PR_Now() / PR_USEC_PER_MSEC - maximumAge_ms) > positionTime_ms;
    if (isTooOld) {
      return;
    }
  }

  nsRefPtr<Position> wrapped;

  if (aPosition) {
    nsCOMPtr<nsIDOMGeoPositionCoords> coords;
    aPosition->GetCoords(getter_AddRefs(coords));
    if (coords) {
      aPosition = ChangeLocation(aPosition);
      if(aPosition) {
        wrapped = new Position(ToSupports(mLocator), aPosition);
      }
    }
  }

  if (!wrapped) {
    NotifyError(nsIDOMGeoPositionError::POSITION_UNAVAILABLE);
    return;
  }

  if (!mIsWatchPositionRequest) {
    // Cancel timer and position updates in case the position
    // callback spins the event loop
    Shutdown();
  }

  nsAutoMicroTask mt;
  if (mCallback.HasWebIDLCallback()) {
    ErrorResult err;
    PositionCallback* callback = mCallback.GetWebIDLCallback();

    MOZ_ASSERT(callback);
    callback->Call(*wrapped, err);
  } else {
    nsIDOMGeoPositionCallback* callback = mCallback.GetXPCOMCallback();

    MOZ_ASSERT(callback);
    callback->HandleEvent(aPosition);
  }

  StopTimeoutTimer();
  MOZ_ASSERT(mShutdown || mIsWatchPositionRequest,
             "non-shutdown getCurrentPosition request after callback!");
}

nsIPrincipal*
nsGeolocationRequest::GetPrincipal()
{
  if (!mLocator) {
    return nullptr;
  }
  return mLocator->GetPrincipal();
}

NS_IMETHODIMP
nsGeolocationRequest::Update(nsIDOMGeoPosition* aPosition)
{
  nsCOMPtr<nsIRunnable> ev = new RequestSendLocationEvent(aPosition, this);
  NS_DispatchToMainThread(ev);
  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationRequest::LocationUpdatePending()
{
  if (!mTimeoutTimer) {
    SetTimeoutTimer();
  }

  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationRequest::NotifyError(uint16_t aErrorCode)
{
  MOZ_ASSERT(NS_IsMainThread());

  nsRefPtr<PositionError> positionError = new PositionError(mLocator, aErrorCode);
  positionError->NotifyCallback(mErrorCallback);
  return NS_OK;
}

void
nsGeolocationRequest::Shutdown()
{
  MOZ_ASSERT(!mShutdown, "request shutdown twice");
  mShutdown = true;

  if (mTimeoutTimer) {
    mTimeoutTimer->Cancel();
    mTimeoutTimer = nullptr;
  }

  // If there are no other high accuracy requests, the geolocation service will
  // notify the provider to switch to the default accuracy.
  if (mOptions && mOptions->mEnableHighAccuracy) {
    nsRefPtr<nsGeolocationService> gs = nsGeolocationService::GetGeolocationService();
    if (gs) {
      gs->UpdateAccuracy();
    }
  }
}

////////////////////////////////////////////////////
// nsGeolocationService
////////////////////////////////////////////////////
NS_INTERFACE_MAP_BEGIN(nsGeolocationService)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIGeolocationUpdate)
  NS_INTERFACE_MAP_ENTRY(nsIGeolocationUpdate)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
NS_INTERFACE_MAP_END

NS_IMPL_ADDREF(nsGeolocationService)
NS_IMPL_RELEASE(nsGeolocationService)


static bool sGeoEnabled = true;
static bool sGeoInitPending = true;
static int32_t sProviderTimeout = 6000; // Time, in milliseconds, to wait for the location provider to spin up.

nsresult nsGeolocationService::Init()
{
  Preferences::AddIntVarCache(&sProviderTimeout, "geo.timeout", sProviderTimeout);
  Preferences::AddBoolVarCache(&sGeoEnabled, "geo.enabled", sGeoEnabled);

  if (!sGeoEnabled) {
    return NS_ERROR_FAILURE;
  }

  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    sGeoInitPending = false;
    return NS_OK;
  }

  // check if the geolocation service is enable from settings
  nsCOMPtr<nsISettingsService> settings =
    do_GetService("@mozilla.org/settingsService;1");

  if (settings) {
    nsCOMPtr<nsISettingsServiceLock> settingsLock;
    nsresult rv = settings->CreateLock(nullptr, getter_AddRefs(settingsLock));
    NS_ENSURE_SUCCESS(rv, rv);

    nsRefPtr<GeolocationSettingsCallback> callback = new GeolocationSettingsCallback();
    rv = settingsLock->Get(GEO_SETINGS_ENABLED, callback);
    NS_ENSURE_SUCCESS(rv, rv);

    callback = new GeolocationSettingsCallback();
    rv = settingsLock->Get(GEO_BLUR_TYPE, callback);
    NS_ENSURE_SUCCESS(rv, rv);

    callback = new GeolocationSettingsCallback();
    rv = settingsLock->Get(GEO_BLUR_RADIUS, callback);
    NS_ENSURE_SUCCESS(rv, rv);

    callback = new GeolocationSettingsCallback();
    rv = settingsLock->Get(GEO_BLUR_COORDS, callback);
    NS_ENSURE_SUCCESS(rv, rv);

    callback = new GeolocationSettingsCallback();
    rv = settingsLock->Get(GEO_EXCEPTIONS, callback);
    NS_ENSURE_SUCCESS(rv, rv);

    callback = new GeolocationSettingsCallback();
    rv = settingsLock->Get(GEO_UNBLURED, callback);
    NS_ENSURE_SUCCESS(rv, rv);
  } else {
    // If we cannot obtain the settings service, we continue
    // assuming that the geolocation is enabled:
    sGeoInitPending = false;
  }

  // geolocation service can be enabled -> now register observer
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    return NS_ERROR_FAILURE;
  }

  obs->AddObserver(this, "quit-application", false);
  obs->AddObserver(this, "mozsettings-changed", false);

#ifdef MOZ_ENABLE_QT5GEOPOSITION
  mProvider = new QTMLocationProvider();
#endif

#ifdef MOZ_WIDGET_ANDROID
  mProvider = new AndroidLocationProvider();
#endif

#ifdef MOZ_WIDGET_GONK
  // GonkGPSGeolocationProvider can be started at boot up time for initialization reasons.
  // do_getService gets hold of the already initialized component and starts
  // processing location requests immediately.
  // do_Createinstance will create multiple instances of the provider which is not right.
  // bug 993041
  mProvider = do_GetService(GONK_GPS_GEOLOCATION_PROVIDER_CONTRACTID);
#endif

#ifdef MOZ_WIDGET_COCOA
  if (Preferences::GetBool("geo.provider.use_corelocation", false)) {
    mProvider = new CoreLocationLocationProvider();
  }
#endif

  if (Preferences::GetBool("geo.provider.use_mls", false)) {
    mProvider = do_CreateInstance("@mozilla.org/geolocation/mls-provider;1");
  }

  // Override platform-specific providers with the default (network)
  // provider while testing. Our tests are currently not meant to exercise
  // the provider, and some tests rely on the network provider being used.
  // "geo.provider.testing" is always set for all plain and browser chrome
  // mochitests, and also for xpcshell tests.
  if (!mProvider || Preferences::GetBool("geo.provider.testing", false)) {
    nsCOMPtr<nsIGeolocationProvider> override =
      do_GetService(NS_GEOLOCATION_PROVIDER_CONTRACTID);

    if (override) {
      mProvider = override;
    }
  }

  return NS_OK;
}

nsGeolocationService::~nsGeolocationService()
{
}

void
nsGeolocationService::HandleMozsettingChanged(const char16_t* aData)
{
    // The string that we're interested in will be a JSON string that looks like:
    //  {"key":"gelocation.enabled","value":true}

    AutoSafeJSContext cx;

    nsDependentString dataStr(aData);
    JS::Rooted<JS::Value> val(cx);
    if (!JS_ParseJSON(cx, dataStr.get(), dataStr.Length(), &val) || !val.isObject()) {
      return;
    }

    JS::Rooted<JSObject*> obj(cx, &val.toObject());
    JS::Rooted<JS::Value> key(cx);
    if (!JS_GetProperty(cx, obj, "key", &key) || !key.isString()) {
      return;
    }

    bool match;
    JS::Rooted<JS::Value> value(cx);

    if (JS_StringEqualsAscii(cx, key.toString(), GEO_SETINGS_ENABLED, &match) && match) {
      if (JS_GetProperty(cx, obj, "value", &value) && value.isBoolean()) {
        HandleMozsettingValue(value.toBoolean());
      }
    }

    if (JS_StringEqualsAscii(cx, key.toString(), GEO_BLUR_TYPE, &match) && match) {
      if (JS_GetProperty(cx, obj, "value", &value) && value.isString()) {
        JSString* jsStr = value.toString();
        if (jsStr) {
          nsString str = EmptyString();
          AutoSafeJSContext cx;
          AssignJSString(cx, str, jsStr);
          int32_t res = 0;
          if(str.EqualsASCII(GEO_BLUR_TYPE_PRECISE_S)) {
            res = GEO_BLUR_TYPE_PRECISE;
          } else if(str.EqualsASCII(GEO_BLUR_TYPE_BLUR_S)) {
            res = GEO_BLUR_TYPE_BLUR;
          } else if(str.EqualsASCII(GEO_BLUR_TYPE_CUSTOM_S)) {
            res = GEO_BLUR_TYPE_CUSTOM;
          } else if(str.EqualsASCII(GEO_BLUR_TYPE_NO_LOCATION_S)) {
            res = GEO_BLUR_TYPE_NO_LOCATION;
          }
          if(res > 0) {
            HandleMozsettingBlurTypeValue(res);
          }
        }
      }
    }

    if (JS_StringEqualsAscii(cx, key.toString(), GEO_BLUR_RADIUS, &match) && match) {
      if (JS_GetProperty(cx, obj, "value", &value) && value.isInt32()) {
        HandleMozsettingRadiusValue(value.toInt32());
      }
    }

    if (JS_StringEqualsAscii(cx, key.toString(), GEO_BLUR_COORDS, &match) && match) {
      if (JS_GetProperty(cx, obj, "value", &value) && value.isString()) {
        HandleMozsettingCoordsValue(value.toString());
      }
    }

    if (JS_StringEqualsAscii(cx, key.toString(), GEO_EXCEPTIONS, &match) && match) {
      if (JS_GetProperty(cx, obj, "value", &value) && value.isObject()) {
         HandleMozsettingExceptionsAppsValue(&value.toObject());
      }
    }

    if (JS_StringEqualsAscii(cx, key.toString(), GEO_UNBLURED, &match) && match) {
      if (JS_GetProperty(cx, obj, "value", &value) && value.isObject()) {
         HandleMozsettingUnbluredAppsValue(&value.toObject());
      }
    }
}

void
nsGeolocationService::HandleMozsettingValue(const bool aValue)
{
    if (!aValue) {
      // turn things off
      StopDevice();
      Update(nullptr);
      mLastPosition.position = nullptr;
      sGeoEnabled = false;
    } else {
      sGeoEnabled = true;
    }

    if (sGeoInitPending) {
      sGeoInitPending = false;
      for (uint32_t i = 0, length = mGeolocators.Length(); i < length; ++i) {
        mGeolocators[i]->ServiceReady();
      }
    }
}

void
nsGeolocationService::HandleMozsettingBlurTypeValue(int32_t aValue)
{
  sGlobalBlurSettings.SetBlurType(aValue);
}

void
nsGeolocationService::HandleMozsettingRadiusValue(int32_t aValue)
{
  sGlobalBlurSettings.ClearCoords();
  sGlobalBlurSettings.SetRadius(aValue);
}

void
nsGeolocationService::HandleMozsettingExceptionsAppsValue(JSObject* aValue)
{
  sExceptionsAppBlurSettings.Clear();

  if(!aValue)
   return;

  AutoSafeJSContext cx;

  JS::RootedObject obj(cx, aValue);
  JS::AutoIdArray ids(cx, JS_Enumerate(cx, obj));

  if (!ids)
      return;

  JS::RootedId id(cx);
  JS::RootedValue propertyValue(cx);

  for (size_t i = 0; i < ids.length(); i++) {
    id = ids[i];

    nsGeoBlurSettings appSettings;

    JS::RootedValue v(cx);
    if (!JS_IdToValue(cx, id, &v))
      continue;

    if (v.isString()) {
      JS::RootedString str(cx, JS::ToString(cx, v));

      if (!str)
        continue;

      nsString manifestURL;

      if (!AssignJSString(cx, manifestURL, str))
        continue;

      appSettings.SetManifestURL(manifestURL);
    }
    else {
      continue;
    }

    if (!JS_GetPropertyById(cx, obj, id, &propertyValue))
      continue;

    if(propertyValue.isObject()) {
      JS::RootedObject settingObj(cx, &propertyValue.toObject());

      JS::RootedValue blurType(cx);
      if (JS_GetProperty(cx, settingObj, "type", &blurType) && blurType.isInt32())
        appSettings.SetBlurType(blurType.toInt32());

      JS::RootedValue radius(cx);
      if (JS_GetProperty(cx, settingObj, "radius", &radius) && radius.isInt32())
        appSettings.SetRadius(radius.toInt32());

      JS::RootedValue coords(cx);
      if (JS_GetProperty(cx, settingObj, "coords", &coords) && coords.isString()) {
        nsString coordsString;

        if (AssignJSString(cx, coordsString, coords.toString())) {
          appSettings.SetCoords(coordsString);
        }
      }

      sExceptionsAppBlurSettings.AppendElement(appSettings);
    }
  }
}
void
nsGeolocationService::HandleMozsettingUnbluredAppsValue(JSObject* aValue)
{
  sUnbluredApps.Clear();

  if(!aValue)
   return;

  AutoSafeJSContext cx;

  JS::RootedObject obj(cx, aValue);
  if(!JS_IsArrayObject(cx, obj))
    return;

  uint32_t length;
  if (!JS_GetArrayLength(cx, obj, &length))
    return;

  for (uint32_t i = 0; i < length; ++i) {
    JS::RootedValue value(cx);

    if (!JS_GetElement(cx, obj, i, &value) || !value.isString())
      continue;

    nsString manifestUrl;
    if (!AssignJSString(cx, manifestUrl, value.toString()))
      continue;

    sUnbluredApps.AppendElement(manifestUrl);
  }
}
void
nsGeolocationService::HandleMozsettingCoordsValue(JSString* aValue)
{
  nsString str;
  if(aValue)
  {
    AutoSafeJSContext cx;
    AssignJSString(cx, str, aValue);
  }

  sGlobalBlurSettings.SetCoords(str);
}

NS_IMETHODIMP
nsGeolocationService::Observe(nsISupports* aSubject,
                              const char* aTopic,
                              const char16_t* aData)
{
  if (!strcmp("quit-application", aTopic)) {
    nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
    if (obs) {
      obs->RemoveObserver(this, "quit-application");
      obs->RemoveObserver(this, "mozsettings-changed");
    }

    for (uint32_t i = 0; i< mGeolocators.Length(); i++) {
      mGeolocators[i]->Shutdown();
    }
    StopDevice();

    return NS_OK;
  }

  if (!strcmp("mozsettings-changed", aTopic)) {
    HandleMozsettingChanged(aData);
    return NS_OK;
  }

  if (!strcmp("timer-callback", aTopic)) {
    // decide if we can close down the service.
    for (uint32_t i = 0; i< mGeolocators.Length(); i++)
      if (mGeolocators[i]->HasActiveCallbacks()) {
        SetDisconnectTimer();
        return NS_OK;
      }

    // okay to close up.
    StopDevice();
    Update(nullptr);
    return NS_OK;
  }

  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
nsGeolocationService::Update(nsIDOMGeoPosition *aSomewhere)
{
  SetCachedPosition(aSomewhere);

  for (uint32_t i = 0; i< mGeolocators.Length(); i++) {
    mGeolocators[i]->Update(aSomewhere);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationService::LocationUpdatePending()
{
  for (uint32_t i = 0; i< mGeolocators.Length(); i++) {
    mGeolocators[i]->LocationUpdatePending();
  }

  return NS_OK;
}

NS_IMETHODIMP
nsGeolocationService::NotifyError(uint16_t aErrorCode)
{
  for (uint32_t i = 0; i < mGeolocators.Length(); i++) {
    mGeolocators[i]->NotifyError(aErrorCode);
  }

  return NS_OK;
}

void
nsGeolocationService::SetCachedPosition(nsIDOMGeoPosition* aPosition)
{
  mLastPosition.position = aPosition;
  mLastPosition.isHighAccuracy = mHigherAccuracy;
}

CachedPositionAndAccuracy
nsGeolocationService::GetCachedPosition()
{
  return mLastPosition;
}

nsresult
nsGeolocationService::StartDevice(nsIPrincipal *aPrincipal)
{
  if (!sGeoEnabled || sGeoInitPending) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // we do not want to keep the geolocation devices online
  // indefinitely.  Close them down after a reasonable period of
  // inactivivity
  SetDisconnectTimer();

  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    ContentChild* cpc = ContentChild::GetSingleton();
    cpc->SendAddGeolocationListener(IPC::Principal(aPrincipal),
                                    HighAccuracyRequested());
    return NS_OK;
  }

  // Start them up!
  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    return NS_ERROR_FAILURE;
  }

  if (!mProvider) {
    return NS_ERROR_FAILURE;
  }

  nsresult rv;

  if (NS_FAILED(rv = mProvider->Startup()) ||
      NS_FAILED(rv = mProvider->Watch(this))) {

    NotifyError(nsIDOMGeoPositionError::POSITION_UNAVAILABLE);
    return rv;
  }

  obs->NotifyObservers(mProvider,
                       "geolocation-device-events",
                       MOZ_UTF16("starting"));

  return NS_OK;
}

void
nsGeolocationService::SetDisconnectTimer()
{
  if (!mDisconnectTimer) {
    mDisconnectTimer = do_CreateInstance("@mozilla.org/timer;1");
  } else {
    mDisconnectTimer->Cancel();
  }

  mDisconnectTimer->Init(this,
                         sProviderTimeout,
                         nsITimer::TYPE_ONE_SHOT);
}

bool
nsGeolocationService::HighAccuracyRequested()
{
  for (uint32_t i = 0; i < mGeolocators.Length(); i++) {
    if (mGeolocators[i]->HighAccuracyRequested()) {
      return true;
    }
  }
  return false;
}

void
nsGeolocationService::UpdateAccuracy(bool aForceHigh)
{
  bool highRequired = aForceHigh || HighAccuracyRequested();

  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    ContentChild* cpc = ContentChild::GetSingleton();
    cpc->SendSetGeolocationHigherAccuracy(highRequired);
    return;
  }

  if (!mHigherAccuracy && highRequired) {
      mProvider->SetHighAccuracy(true);
  }

  if (mHigherAccuracy && !highRequired) {
      mProvider->SetHighAccuracy(false);
  }

  mHigherAccuracy = highRequired;
}

void
nsGeolocationService::StopDevice()
{
  if(mDisconnectTimer) {
    mDisconnectTimer->Cancel();
    mDisconnectTimer = nullptr;
  }

  if (XRE_GetProcessType() == GeckoProcessType_Content) {
    ContentChild* cpc = ContentChild::GetSingleton();
    cpc->SendRemoveGeolocationListener();
    return; // bail early
  }

  nsCOMPtr<nsIObserverService> obs = services::GetObserverService();
  if (!obs) {
    return;
  }

  if (!mProvider) {
    return;
  }

  mHigherAccuracy = false;

  mProvider->Shutdown();
  obs->NotifyObservers(mProvider,
                       "geolocation-device-events",
                       MOZ_UTF16("shutdown"));
}

StaticRefPtr<nsGeolocationService> nsGeolocationService::sService;

already_AddRefed<nsGeolocationService>
nsGeolocationService::GetGeolocationService()
{
  nsRefPtr<nsGeolocationService> result;
  if (nsGeolocationService::sService) {
    result = nsGeolocationService::sService;
    return result.forget();
  }

  result = new nsGeolocationService();
  if (NS_FAILED(result->Init())) {
    return nullptr;
  }
  ClearOnShutdown(&nsGeolocationService::sService);
  nsGeolocationService::sService = result;
  return result.forget();
}

void
nsGeolocationService::AddLocator(Geolocation* aLocator)
{
  mGeolocators.AppendElement(aLocator);
}

void
nsGeolocationService::RemoveLocator(Geolocation* aLocator)
{
  mGeolocators.RemoveElement(aLocator);
}

////////////////////////////////////////////////////
// Geolocation
////////////////////////////////////////////////////

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(Geolocation)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIDOMGeoGeolocation)
  NS_INTERFACE_MAP_ENTRY(nsIDOMGeoGeolocation)
  NS_INTERFACE_MAP_ENTRY(nsIGeolocationUpdate)
NS_INTERFACE_MAP_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(Geolocation)
NS_IMPL_CYCLE_COLLECTING_RELEASE(Geolocation)

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE(Geolocation,
                                      mPendingCallbacks,
                                      mWatchingCallbacks,
                                      mPendingRequests)

Geolocation::Geolocation()
: mLastWatchId(0)
{
  SetIsDOMBinding();
}

Geolocation::~Geolocation()
{
  if (mService) {
    Shutdown();
  }
}

nsresult
Geolocation::Init(nsIDOMWindow* aContentDom)
{
  // Remember the window
  if (aContentDom) {
    nsCOMPtr<nsPIDOMWindow> window = do_QueryInterface(aContentDom);
    if (!window) {
      return NS_ERROR_FAILURE;
    }

    mOwner = do_GetWeakReference(window->GetCurrentInnerWindow());
    if (!mOwner) {
      return NS_ERROR_FAILURE;
    }

    // Grab the principal of the document
    nsCOMPtr<nsIDocument> doc = window->GetDoc();
    if (!doc) {
      return NS_ERROR_FAILURE;
    }

    mPrincipal = doc->NodePrincipal();
  }

  // If no aContentDom was passed into us, we are being used
  // by chrome/c++ and have no mOwner, no mPrincipal, and no need
  // to prompt.
  mService = nsGeolocationService::GetGeolocationService();
  if (mService) {
    mService->AddLocator(this);
  }
  return NS_OK;
}

void
Geolocation::Shutdown()
{
  // Release all callbacks
  mPendingCallbacks.Clear();
  mWatchingCallbacks.Clear();

  if (mService) {
    mService->RemoveLocator(this);
    mService->UpdateAccuracy();
  }

  mService = nullptr;
  mPrincipal = nullptr;
}

nsIDOMWindow*
Geolocation::GetParentObject() const {
  nsCOMPtr<nsPIDOMWindow> window = do_QueryReferent(mOwner);
  return window.get();
}

bool
Geolocation::HasActiveCallbacks()
{
  return mPendingCallbacks.Length() || mWatchingCallbacks.Length();
}

bool
Geolocation::HighAccuracyRequested()
{
  for (uint32_t i = 0; i < mWatchingCallbacks.Length(); i++) {
    if (mWatchingCallbacks[i]->WantsHighAccuracy()) {
      return true;
    }
  }

  for (uint32_t i = 0; i < mPendingCallbacks.Length(); i++) {
    if (mPendingCallbacks[i]->WantsHighAccuracy()) {
      return true;
    }
  }

  return false;
}

void
Geolocation::RemoveRequest(nsGeolocationRequest* aRequest)
{
  bool requestWasKnown =
    (mPendingCallbacks.RemoveElement(aRequest) !=
     mWatchingCallbacks.RemoveElement(aRequest));

  unused << requestWasKnown;
}

NS_IMETHODIMP
Geolocation::Update(nsIDOMGeoPosition *aSomewhere)
{
  if (!WindowOwnerStillExists()) {
    Shutdown();
    return NS_OK;
  }

  if (aSomewhere) {
    nsCOMPtr<nsIDOMGeoPositionCoords> coords;
    aSomewhere->GetCoords(getter_AddRefs(coords));
    if (coords) {
      double accuracy = -1;
      coords->GetAccuracy(&accuracy);
      mozilla::Telemetry::Accumulate(mozilla::Telemetry::GEOLOCATION_ACCURACY, accuracy);
    }
  }

  for (uint32_t i = mPendingCallbacks.Length(); i > 0; i--) {
    mPendingCallbacks[i-1]->Update(aSomewhere);
    RemoveRequest(mPendingCallbacks[i-1]);
  }

  // notify everyone that is watching
  for (uint32_t i = 0; i < mWatchingCallbacks.Length(); i++) {
    mWatchingCallbacks[i]->Update(aSomewhere);
  }

  return NS_OK;
}

NS_IMETHODIMP
Geolocation::LocationUpdatePending()
{
  // this event is only really interesting for watch callbacks
  for (uint32_t i = 0; i < mWatchingCallbacks.Length(); i++) {
    mWatchingCallbacks[i]->LocationUpdatePending();
  }

  return NS_OK;
}

NS_IMETHODIMP
Geolocation::NotifyError(uint16_t aErrorCode)
{
  if (!WindowOwnerStillExists()) {
    Shutdown();
    return NS_OK;
  }

  mozilla::Telemetry::Accumulate(mozilla::Telemetry::GEOLOCATION_ERROR, true);

  for (uint32_t i = mPendingCallbacks.Length(); i > 0; i--) {
    mPendingCallbacks[i-1]->NotifyErrorAndShutdown(aErrorCode);
    //NotifyErrorAndShutdown() removes the request from the array
  }

  // notify everyone that is watching
  for (uint32_t i = 0; i < mWatchingCallbacks.Length(); i++) {
    mWatchingCallbacks[i]->NotifyErrorAndShutdown(aErrorCode);
  }

  return NS_OK;
}

void
Geolocation::GetCurrentPosition(PositionCallback& aCallback,
                                PositionErrorCallback* aErrorCallback,
                                const PositionOptions& aOptions,
                                ErrorResult& aRv)
{
  GeoPositionCallback successCallback(&aCallback);
  GeoPositionErrorCallback errorCallback(aErrorCallback);

  nsresult rv = GetCurrentPosition(successCallback, errorCallback,
                                   CreatePositionOptionsCopy(aOptions));

  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }

  return;
}

NS_IMETHODIMP
Geolocation::GetCurrentPosition(nsIDOMGeoPositionCallback* aCallback,
                                nsIDOMGeoPositionErrorCallback* aErrorCallback,
                                PositionOptions* aOptions)
{
  NS_ENSURE_ARG_POINTER(aCallback);

  GeoPositionCallback successCallback(aCallback);
  GeoPositionErrorCallback errorCallback(aErrorCallback);

  return GetCurrentPosition(successCallback, errorCallback, aOptions);
}

nsresult
Geolocation::GetCurrentPosition(GeoPositionCallback& callback,
                                GeoPositionErrorCallback& errorCallback,
                                PositionOptions *options)
{
  if (mPendingCallbacks.Length() > MAX_GEO_REQUESTS_PER_WINDOW) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  nsRefPtr<nsGeolocationRequest> request = new nsGeolocationRequest(this,
                                                                    callback,
                                                                    errorCallback,
                                                                    options,
                                                                    false);

  if (!sGeoEnabled) {
    nsCOMPtr<nsIRunnable> ev = new RequestAllowEvent(false, request);
    NS_DispatchToMainThread(ev);
    return NS_OK;
  }

  if (!mOwner && !nsContentUtils::IsCallerChrome()) {
    return NS_ERROR_FAILURE;
  }

  if (sGeoInitPending) {
    mPendingRequests.AppendElement(request);
    return NS_OK;
  }

  return GetCurrentPositionReady(request);
}

nsresult
Geolocation::GetCurrentPositionReady(nsGeolocationRequest* aRequest)
{
  if (mOwner) {
    if (!RegisterRequestWithPrompt(aRequest)) {
      return NS_ERROR_NOT_AVAILABLE;
    }

    return NS_OK;
  }

  if (!nsContentUtils::IsCallerChrome()) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIRunnable> ev = new RequestAllowEvent(true, aRequest);
  NS_DispatchToMainThread(ev);

  return NS_OK;
}

int32_t
Geolocation::WatchPosition(PositionCallback& aCallback,
                           PositionErrorCallback* aErrorCallback,
                           const PositionOptions& aOptions,
                           ErrorResult& aRv)
{
  int32_t ret;
  GeoPositionCallback successCallback(&aCallback);
  GeoPositionErrorCallback errorCallback(aErrorCallback);

  nsresult rv = WatchPosition(successCallback, errorCallback,
                              CreatePositionOptionsCopy(aOptions), &ret);

  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
  }

  return ret;
}

NS_IMETHODIMP
Geolocation::WatchPosition(nsIDOMGeoPositionCallback *aCallback,
                           nsIDOMGeoPositionErrorCallback *aErrorCallback,
                           PositionOptions *aOptions,
                           int32_t* aRv)
{
  NS_ENSURE_ARG_POINTER(aCallback);

  GeoPositionCallback successCallback(aCallback);
  GeoPositionErrorCallback errorCallback(aErrorCallback);

  return WatchPosition(successCallback, errorCallback, aOptions, aRv);
}

nsresult
Geolocation::WatchPosition(GeoPositionCallback& aCallback,
                           GeoPositionErrorCallback& aErrorCallback,
                           PositionOptions* aOptions,
                           int32_t* aRv)
{
  if (mWatchingCallbacks.Length() > MAX_GEO_REQUESTS_PER_WINDOW) {
    return NS_ERROR_NOT_AVAILABLE;
  }

  // The watch ID:
  *aRv = mLastWatchId++;

  nsRefPtr<nsGeolocationRequest> request = new nsGeolocationRequest(this,
                                                                    aCallback,
                                                                    aErrorCallback,
                                                                    aOptions,
                                                                    true,
                                                                    *aRv);

  if (!sGeoEnabled) {
    nsCOMPtr<nsIRunnable> ev = new RequestAllowEvent(false, request);
    NS_DispatchToMainThread(ev);
    return NS_OK;
  }

  if (!mOwner && !nsContentUtils::IsCallerChrome()) {
    return NS_ERROR_FAILURE;
  }

  if (sGeoInitPending) {
    mPendingRequests.AppendElement(request);
    return NS_OK;
  }

  return WatchPositionReady(request);
}

nsresult
Geolocation::WatchPositionReady(nsGeolocationRequest* aRequest)
{
  if (mOwner) {
    if (!RegisterRequestWithPrompt(aRequest))
      return NS_ERROR_NOT_AVAILABLE;

    return NS_OK;
  }

  if (!nsContentUtils::IsCallerChrome()) {
    return NS_ERROR_FAILURE;
  }

  aRequest->Allow(JS::UndefinedHandleValue);

  return NS_OK;
}

NS_IMETHODIMP
Geolocation::ClearWatch(int32_t aWatchId)
{
  if (aWatchId < 0) {
    return NS_OK;
  }

  for (uint32_t i = 0, length = mWatchingCallbacks.Length(); i < length; ++i) {
    if (mWatchingCallbacks[i]->WatchId() == aWatchId) {
      mWatchingCallbacks[i]->Shutdown();
      RemoveRequest(mWatchingCallbacks[i]);
      break;
    }
  }

  // make sure we also search through the pending requests lists for
  // watches to clear...
  for (uint32_t i = 0, length = mPendingRequests.Length(); i < length; ++i) {
    if (mPendingRequests[i]->IsWatch() &&
        (mPendingRequests[i]->WatchId() == aWatchId)) {
      mPendingRequests[i]->Shutdown();
      mPendingRequests.RemoveElementAt(i);
      break;
    }
  }

  return NS_OK;
}

void
Geolocation::ServiceReady()
{
  for (uint32_t length = mPendingRequests.Length(); length > 0; --length) {
    if (mPendingRequests[0]->IsWatch()) {
      WatchPositionReady(mPendingRequests[0]);
    } else {
      GetCurrentPositionReady(mPendingRequests[0]);
    }

    mPendingRequests.RemoveElementAt(0);
  }
}

bool
Geolocation::WindowOwnerStillExists()
{
  // an owner was never set when Geolocation
  // was created, which means that this object
  // is being used without a window.
  if (mOwner == nullptr) {
    return true;
  }

  nsCOMPtr<nsPIDOMWindow> window = do_QueryReferent(mOwner);

  if (window) {
    bool closed = false;
    window->GetClosed(&closed);
    if (closed) {
      return false;
    }

    nsPIDOMWindow* outer = window->GetOuterWindow();
    if (!outer || outer->GetCurrentInnerWindow() != window) {
      return false;
    }
  }

  return true;
}

void
Geolocation::NotifyAllowedRequest(nsGeolocationRequest* aRequest)
{
  if (aRequest->IsWatch()) {
    mWatchingCallbacks.AppendElement(aRequest);
  } else {
    mPendingCallbacks.AppendElement(aRequest);
  }
}

bool
Geolocation::RegisterRequestWithPrompt(nsGeolocationRequest* request)
{
  if (Preferences::GetBool("geo.prompt.testing", false)) {
    bool allow = Preferences::GetBool("geo.prompt.testing.allow", false);
    nsCOMPtr<nsIRunnable> ev = new RequestAllowEvent(allow,
						     request);
    NS_DispatchToMainThread(ev);
    return true;
  }

  nsCOMPtr<nsIRunnable> ev  = new RequestPromptEvent(request, mOwner);
  NS_DispatchToMainThread(ev);
  return true;
}

NS_IMETHODIMP
Geolocation::SetManifestURL(const nsAString& aManifestURL)
{
  mManifestURL = aManifestURL;
  return NS_OK;
}

nsString
Geolocation::GetManifestURL()
{
  return mManifestURL;
}


JSObject*
Geolocation::WrapObject(JSContext *aCtx)
{
  return GeolocationBinding::Wrap(aCtx, this);
}
