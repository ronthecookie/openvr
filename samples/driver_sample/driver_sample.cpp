//============ Copyright (c) Valve Corporation, All rights reserved. ============

#include <openvr_driver.h>
#include "driverlog.h"
// #include "systemtime.h"
#include <math.h>
#include <vector>
#include <thread>
#include <chrono>

#if defined(_WINDOWS)
#include <windows.h>
#endif

using namespace vr;

#if defined(_WIN32)
#define HMD_DLL_EXPORT extern "C" __declspec(dllexport)
#define HMD_DLL_IMPORT extern "C" __declspec(dllimport)
#elif defined(__GNUC__) || defined(COMPILER_GCC) || defined(__APPLE__)
#define HMD_DLL_EXPORT extern "C" __attribute__((visibility("default")))
#define HMD_DLL_IMPORT extern "C"
#else
#error "Unsupported Platform."
#endif
#define ARRAYSIZE(a)              \
	((sizeof(a) / sizeof(*(a))) / \
	 static_cast<size_t>(!(sizeof(a) % sizeof(*(a)))))

static const char *const k_pch_VirtualDisplay_Section = "driver_virtual_display";
static const char *const k_pch_VirtualDisplay_SerialNumber_String = "serialNumber";
static const char *const k_pch_VirtualDisplay_ModelNumber_String = "modelNumber";
static const char *const k_pch_VirtualDisplay_AdditionalLatencyInSeconds_Float = "additionalLatencyInSeconds";
static const char *const k_pch_VirtualDisplay_DisplayWidth_Int32 = "displayWidth";
static const char *const k_pch_VirtualDisplay_DisplayHeight_Int32 = "displayHeight";
static const char *const k_pch_VirtualDisplay_DisplayRefreshRateNumerator_Int32 = "displayRefreshRateNumerator";
static const char *const k_pch_VirtualDisplay_DisplayRefreshRateDenominator_Int32 = "displayRefreshRateDenominator";
static const char *const k_pch_VirtualDisplay_AdapterIndex_Int32 = "adapterIndex";
inline HmdQuaternion_t HmdQuaternion_Init(double w, double x, double y, double z)
{
	HmdQuaternion_t quat;
	quat.w = w;
	quat.x = x;
	quat.y = y;
	quat.z = z;
	return quat;
}

inline void HmdMatrix_SetIdentity(HmdMatrix34_t *pMatrix)
{
	pMatrix->m[0][0] = 1.f;
	pMatrix->m[0][1] = 0.f;
	pMatrix->m[0][2] = 0.f;
	pMatrix->m[0][3] = 0.f;
	pMatrix->m[1][0] = 0.f;
	pMatrix->m[1][1] = 1.f;
	pMatrix->m[1][2] = 0.f;
	pMatrix->m[1][3] = 0.f;
	pMatrix->m[2][0] = 0.f;
	pMatrix->m[2][1] = 0.f;
	pMatrix->m[2][2] = 1.f;
	pMatrix->m[2][3] = 0.f;
}

// keys for use with the settings API
static const char *const k_pch_Sample_Section = "driver_sample";
static const char *const k_pch_Sample_SerialNumber_String = "serialNumber";
static const char *const k_pch_Sample_ModelNumber_String = "modelNumber";
static const char *const k_pch_Sample_WindowX_Int32 = "windowX";
static const char *const k_pch_Sample_WindowY_Int32 = "windowY";
static const char *const k_pch_Sample_WindowWidth_Int32 = "windowWidth";
static const char *const k_pch_Sample_WindowHeight_Int32 = "windowHeight";
static const char *const k_pch_Sample_RenderWidth_Int32 = "renderWidth";
static const char *const k_pch_Sample_RenderHeight_Int32 = "renderHeight";
static const char *const k_pch_Sample_SecondsFromVsyncToPhotons_Float = "secondsFromVsyncToPhotons";
static const char *const k_pch_Sample_DisplayFrequency_Float = "displayFrequency";

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------

class CWatchdogDriver_Sample : public IVRWatchdogProvider
{
public:
	CWatchdogDriver_Sample()
	{
		m_pWatchdogThread = nullptr;
	}

	virtual EVRInitError Init(vr::IVRDriverContext *pDriverContext);
	virtual void Cleanup();

private:
	std::thread *m_pWatchdogThread;
};

CWatchdogDriver_Sample g_watchdogDriverNull;

bool g_bExiting = false;

void WatchdogThreadFunction()
{
	while (!g_bExiting)
	{
#if defined(_WINDOWS)
		// on windows send the event when the Y key is pressed.
		if ((0x01 & GetAsyncKeyState('Y')) != 0)
		{
			// Y key was pressed.
			vr::VRWatchdogHost()->WatchdogWakeUp(vr::TrackedDeviceClass_HMD);
		}
		std::this_thread::sleep_for(std::chrono::microseconds(500));
#else
		// for the other platforms, just send one every five seconds
		std::this_thread::sleep_for(std::chrono::seconds(5));
		vr::VRWatchdogHost()->WatchdogWakeUp(vr::TrackedDeviceClass_HMD);
#endif
	}
}

EVRInitError CWatchdogDriver_Sample::Init(vr::IVRDriverContext *pDriverContext)
{
	VR_INIT_WATCHDOG_DRIVER_CONTEXT(pDriverContext);
	InitDriverLog(vr::VRDriverLog());

	// Watchdog mode on Windows starts a thread that listens for the 'Y' key on the keyboard to
	// be pressed. A real driver should wait for a system button event or something else from the
	// the hardware that signals that the VR system should start up.
	g_bExiting = false;
	m_pWatchdogThread = new std::thread(WatchdogThreadFunction);
	if (!m_pWatchdogThread)
	{
		DriverLog("Unable to create watchdog thread\n");
		return VRInitError_Driver_Failed;
	}

	return VRInitError_None;
}

void CWatchdogDriver_Sample::Cleanup()
{
	g_bExiting = true;
	if (m_pWatchdogThread)
	{
		m_pWatchdogThread->join();
		delete m_pWatchdogThread;
		m_pWatchdogThread = nullptr;
	}

	CleanupDriverLog();
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CSampleDeviceDriver : public vr::ITrackedDeviceServerDriver, public vr::IVRDisplayComponent
{
public:
	CSampleDeviceDriver()
	{
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
		m_ulPropertyContainer = vr::k_ulInvalidPropertyContainer;

		DriverLog("Using settings values\n");
		m_flIPD = vr::VRSettings()->GetFloat(k_pch_SteamVR_Section, k_pch_SteamVR_IPD_Float);

		char buf[1024];
		vr::VRSettings()->GetString(k_pch_Sample_Section, k_pch_Sample_SerialNumber_String, buf, sizeof(buf));
		m_sSerialNumber = buf;

		vr::VRSettings()->GetString(k_pch_Sample_Section, k_pch_Sample_ModelNumber_String, buf, sizeof(buf));
		m_sModelNumber = buf;

		m_nWindowX = vr::VRSettings()->GetInt32(k_pch_Sample_Section, k_pch_Sample_WindowX_Int32);
		m_nWindowY = vr::VRSettings()->GetInt32(k_pch_Sample_Section, k_pch_Sample_WindowY_Int32);
		m_nWindowWidth = vr::VRSettings()->GetInt32(k_pch_Sample_Section, k_pch_Sample_WindowWidth_Int32);
		m_nWindowHeight = vr::VRSettings()->GetInt32(k_pch_Sample_Section, k_pch_Sample_WindowHeight_Int32);
		m_nRenderWidth = vr::VRSettings()->GetInt32(k_pch_Sample_Section, k_pch_Sample_RenderWidth_Int32);
		m_nRenderHeight = vr::VRSettings()->GetInt32(k_pch_Sample_Section, k_pch_Sample_RenderHeight_Int32);
		m_flSecondsFromVsyncToPhotons = vr::VRSettings()->GetFloat(k_pch_Sample_Section, k_pch_Sample_SecondsFromVsyncToPhotons_Float);
		m_flDisplayFrequency = vr::VRSettings()->GetFloat(k_pch_Sample_Section, k_pch_Sample_DisplayFrequency_Float);

		DriverLog("driver_null: Serial Number: %s\n", m_sSerialNumber.c_str());
		DriverLog("driver_null: Model Number: %s\n", m_sModelNumber.c_str());
		DriverLog("driver_null: Window: %d %d %d %d\n", m_nWindowX, m_nWindowY, m_nWindowWidth, m_nWindowHeight);
		DriverLog("driver_null: Render Target: %d %d\n", m_nRenderWidth, m_nRenderHeight);
		DriverLog("driver_null: Seconds from Vsync to Photons: %f\n", m_flSecondsFromVsyncToPhotons);
		DriverLog("driver_null: Display Frequency: %f\n", m_flDisplayFrequency);
		DriverLog("driver_null: IPD: %f\n", m_flIPD);
	}

	virtual ~CSampleDeviceDriver()
	{
	}

	virtual EVRInitError Activate(vr::TrackedDeviceIndex_t unObjectId)
	{
		m_unObjectId = unObjectId;
		m_ulPropertyContainer = vr::VRProperties()->TrackedDeviceToPropertyContainer(m_unObjectId);

		vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, Prop_ModelNumber_String, m_sModelNumber.c_str());
		vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, Prop_RenderModelName_String, m_sModelNumber.c_str());
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_UserIpdMeters_Float, m_flIPD);
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_UserHeadToEyeDepthMeters_Float, 0.f);
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_DisplayFrequency_Float, m_flDisplayFrequency);
		vr::VRProperties()->SetFloatProperty(m_ulPropertyContainer, Prop_SecondsFromVsyncToPhotons_Float, m_flSecondsFromVsyncToPhotons);

		// return a constant that's not 0 (invalid) or 1 (reserved for Oculus)
		vr::VRProperties()->SetUint64Property(m_ulPropertyContainer, Prop_CurrentUniverseId_Uint64, 2);

		// avoid "not fullscreen" warnings from vrmonitor
		vr::VRProperties()->SetBoolProperty(m_ulPropertyContainer, Prop_IsOnDesktop_Bool, false);

		// Icons can be configured in code or automatically configured by an external file "drivername\resources\driver.vrresources".
		// Icon properties NOT configured in code (post Activate) are then auto-configured by the optional presence of a driver's "drivername\resources\driver.vrresources".
		// In this manner a driver can configure their icons in a flexible data driven fashion by using an external file.
		//
		// The structure of the driver.vrresources file allows a driver to specialize their icons based on their HW.
		// Keys matching the value in "Prop_ModelNumber_String" are considered first, since the driver may have model specific icons.
		// An absence of a matching "Prop_ModelNumber_String" then considers the ETrackedDeviceClass ("HMD", "Controller", "GenericTracker", "TrackingReference")
		// since the driver may have specialized icons based on those device class names.
		//
		// An absence of either then falls back to the "system.vrresources" where generic device class icons are then supplied.
		//
		// Please refer to "bin\drivers\sample\resources\driver.vrresources" which contains this sample configuration.
		//
		// "Alias" is a reserved key and specifies chaining to another json block.
		//
		// In this sample configuration file (overly complex FOR EXAMPLE PURPOSES ONLY)....
		//
		// "Model-v2.0" chains through the alias to "Model-v1.0" which chains through the alias to "Model-v Defaults".
		//
		// Keys NOT found in "Model-v2.0" would then chase through the "Alias" to be resolved in "Model-v1.0" and either resolve their or continue through the alias.
		// Thus "Prop_NamedIconPathDeviceAlertLow_String" in each model's block represent a specialization specific for that "model".
		// Keys in "Model-v Defaults" are an example of mapping to the same states, and here all map to "Prop_NamedIconPathDeviceOff_String".
		//
		bool bSetupIconUsingExternalResourceFile = true;
		if (!bSetupIconUsingExternalResourceFile)
		{
			// Setup properties directly in code.
			// Path values are of the form {drivername}\icons\some_icon_filename.png
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceOff_String, "{sample}/icons/headset_sample_status_off.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearching_String, "{sample}/icons/headset_sample_status_searching.gif");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceSearchingAlert_String, "{sample}/icons/headset_sample_status_searching_alert.gif");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReady_String, "{sample}/icons/headset_sample_status_ready.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceReadyAlert_String, "{sample}/icons/headset_sample_status_ready_alert.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceNotReady_String, "{sample}/icons/headset_sample_status_error.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceStandby_String, "{sample}/icons/headset_sample_status_standby.png");
			vr::VRProperties()->SetStringProperty(m_ulPropertyContainer, vr::Prop_NamedIconPathDeviceAlertLow_String, "{sample}/icons/headset_sample_status_ready_low.png");
		}

		return VRInitError_None;
	}

	virtual void Deactivate()
	{
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
	}

	virtual void EnterStandby()
	{
	}

	void *GetComponent(const char *pchComponentNameAndVersion)
	{
		DriverLog("CSampleDeviceDriver#GetComponent(%s)\n", pchComponentNameAndVersion);

		if (!_stricmp(pchComponentNameAndVersion, vr::IVRDisplayComponent_Version))
		{
			return (vr::IVRDisplayComponent *)this;
		}

		// override this to add a component to a driver
		return NULL;
	}

	virtual void PowerOff()
	{
	}

	/** debug request from a client */
	virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize)
	{
		if (unResponseBufferSize >= 1)
			pchResponseBuffer[0] = 0;
	}

	virtual void GetWindowBounds(int32_t *pnX, int32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight)
	{
		*pnX = m_nWindowX;
		*pnY = m_nWindowY;
		*pnWidth = m_nWindowWidth;
		*pnHeight = m_nWindowHeight;
	}

	virtual bool IsDisplayOnDesktop()
	{
		return false;
	}

	virtual bool IsDisplayRealDisplay()
	{
		return false;
	}

	virtual void GetRecommendedRenderTargetSize(uint32_t *pnWidth, uint32_t *pnHeight)
	{
		*pnWidth = m_nRenderWidth;
		*pnHeight = m_nRenderHeight;
	}

	virtual void GetEyeOutputViewport(EVREye eEye, uint32_t *pnX, uint32_t *pnY, uint32_t *pnWidth, uint32_t *pnHeight)
	{
		*pnY = 0;
		*pnWidth = m_nWindowWidth / 2;
		*pnHeight = m_nWindowHeight;

		if (eEye == Eye_Left)
		{
			*pnX = 0;
		}
		else
		{
			*pnX = m_nWindowWidth / 2;
		}
	}

	virtual void GetProjectionRaw(EVREye eEye, float *pfLeft, float *pfRight, float *pfTop, float *pfBottom)
	{
		*pfLeft = -1.0;
		*pfRight = 1.0;
		*pfTop = -1.0;
		*pfBottom = 1.0;
	}

	virtual DistortionCoordinates_t ComputeDistortion(EVREye eEye, float fU, float fV)
	{
		DistortionCoordinates_t coordinates;
		coordinates.rfBlue[0] = fU;
		coordinates.rfBlue[1] = fV;
		coordinates.rfGreen[0] = fU;
		coordinates.rfGreen[1] = fV;
		coordinates.rfRed[0] = fU;
		coordinates.rfRed[1] = fV;
		return coordinates;
	}

	virtual DriverPose_t GetPose()
	{
		DriverPose_t pose = {0};
		pose.poseIsValid = true;
		pose.result = TrackingResult_Running_OK;
		pose.deviceIsConnected = true;

		pose.qWorldFromDriverRotation = HmdQuaternion_Init(1, 0, 0, 0);
		pose.qDriverFromHeadRotation = HmdQuaternion_Init(1, 0, 0, 0);

		return pose;
	}

	void RunFrame()
	{
		// In a real driver, this should happen from some pose tracking thread.
		// The RunFrame interval is unspecified and can be very irregular if some other
		// driver blocks it for some periodic task.
		// DriverLog("frame!");
		if (m_unObjectId != vr::k_unTrackedDeviceIndexInvalid)
		{
			vr::VRServerDriverHost()->TrackedDevicePoseUpdated(m_unObjectId, GetPose(), sizeof(DriverPose_t));
		}
	}

	std::string GetSerialNumber() const { return m_sSerialNumber; }

private:
	vr::TrackedDeviceIndex_t m_unObjectId;
	vr::PropertyContainerHandle_t m_ulPropertyContainer;

	std::string m_sSerialNumber;
	std::string m_sModelNumber;

	int32_t m_nWindowX;
	int32_t m_nWindowY;
	int32_t m_nWindowWidth;
	int32_t m_nWindowHeight;
	int32_t m_nRenderWidth;
	int32_t m_nRenderHeight;
	float m_flSecondsFromVsyncToPhotons;
	float m_flDisplayFrequency;
	float m_flIPD;
};
//-----------------------------------------------------------------------------
// Purpose: This object represents our device (registered below).
// It implements the IVRVirtualDisplay component interface to provide us
// hooks into the render pipeline.
//-----------------------------------------------------------------------------
// class CDisplayRedirectLatest : public vr::ITrackedDeviceServerDriver, public vr::IVRVirtualDisplay
// {
// public:
// 	CDisplayRedirectLatest()
// 		: m_unObjectId(vr::k_unTrackedDeviceIndexInvalid), m_nGraphicsAdapterLuid(0), m_flLastVsyncTimeInSeconds(0.0) /*, m_nVsyncCounter(0), m_pD3DRender(NULL), m_pFlushTexture(NULL), m_pRemoteDevice(NULL), m_pEncoder(NULL)*/
// 	{
// 		vr::VRSettings()->GetString(k_pch_VirtualDisplay_Section,
// 									vr::k_pch_Null_SerialNumber_String, m_rchSerialNumber, ARRAYSIZE(m_rchSerialNumber));
// 		vr::VRSettings()->GetString(k_pch_VirtualDisplay_Section,
// 									vr::k_pch_Null_ModelNumber_String, m_rchModelNumber, ARRAYSIZE(m_rchModelNumber));

// 		m_flAdditionalLatencyInSeconds = std::max(0.0f,
// 												  vr::VRSettings()->GetFloat(k_pch_VirtualDisplay_Section,
// 																			 k_pch_VirtualDisplay_AdditionalLatencyInSeconds_Float));

// 		int32_t nDisplayWidth = vr::VRSettings()->GetInt32(
// 			k_pch_VirtualDisplay_Section,
// 			k_pch_VirtualDisplay_DisplayWidth_Int32);
// 		int32_t nDisplayHeight = vr::VRSettings()->GetInt32(
// 			k_pch_VirtualDisplay_Section,
// 			k_pch_VirtualDisplay_DisplayHeight_Int32);

// 		int32_t nDisplayRefreshRateNumerator = vr::VRSettings()->GetInt32(
// 			k_pch_VirtualDisplay_Section,
// 			k_pch_VirtualDisplay_DisplayRefreshRateNumerator_Int32);
// 		int32_t nDisplayRefreshRateDenominator = vr::VRSettings()->GetInt32(
// 			k_pch_VirtualDisplay_Section,
// 			k_pch_VirtualDisplay_DisplayRefreshRateDenominator_Int32);

// 		int32_t nAdapterIndex = vr::VRSettings()->GetInt32(
// 			k_pch_VirtualDisplay_Section,
// 			k_pch_VirtualDisplay_AdapterIndex_Int32);

// 		// m_pD3DRender = new CD3DRender();

// 		// First initialize using the specified display dimensions to determine
// 		// which graphics adapter the headset is attached to (if any).
// 		// if (!m_pD3DRender->Initialize(nDisplayWidth, nDisplayHeight))
// 		// {
// 		// 	DriverLog("Could not find headset with display size %dx%d.", nDisplayWidth, nDisplayHeight);
// 		// 	return;
// 		// }

// 		int32_t nDisplayX, nDisplayY;
// 		// m_pD3DRender->GetDisplayPos(&nDisplayX, &nDisplayY);

// 		int32_t nDisplayAdapterIndex;
// 		const int32_t nBufferSize = 128;
// 		wchar_t wchAdapterDescription[nBufferSize];
// 		// if (!m_pD3DRender->GetAdapterInfo(&nDisplayAdapterIndex, wchAdapterDescription, nBufferSize))
// 		// {
// 		// 	DriverLog("Failed to get headset adapter info!");
// 		// 	return;
// 		// }

// 		// char chAdapterDescription[nBufferSize];
// 		// wcstombs(0, chAdapterDescription, nBufferSize, wchAdapterDescription, nBufferSize);
// 		DriverLog("Headset connected.");

// 		// If no adapter specified, choose the first one the headset *isn't* plugged into.
// 		if (nAdapterIndex < 0)
// 		{
// 			nAdapterIndex = (nDisplayAdapterIndex == 0) ? 1 : 0;
// 		}
// 		else if (nDisplayAdapterIndex == nAdapterIndex)
// 		{
// 			DriverLog("Headset needs to be plugged into a separate graphics card.");
// 			return;
// 		}

// 		// // Store off the LUID of the primary gpu we want to use.
// 		// if (!m_pD3DRender->GetAdapterLuid(nAdapterIndex, &m_nGraphicsAdapterLuid))
// 		// {
// 		// 	DriverLog("Failed to get adapter index for graphics adapter!");
// 		// 	return;
// 		// }

// 		// // Now reinitialize using the other graphics card.
// 		// if (!m_pD3DRender->Initialize(nAdapterIndex))
// 		// {
// 		// 	DriverLog("Could not create graphics device for adapter %d.  Requires a minimum of two graphics cards.", nAdapterIndex);
// 		// 	return;
// 		// }

// 		// if (!m_pD3DRender->GetAdapterInfo(&nDisplayAdapterIndex, wchAdapterDescription, nBufferSize))
// 		// {
// 		// 	DriverLog("Failed to get primary adapter info!");
// 		// 	return;
// 		// }

// 		// wcstombs(0, chAdapterDescription, nBufferSize, wchAdapterDescription, nBufferSize);
// 		DriverLog("Using <> as primary graphics adapter.");

// 		// Spawn our separate process to manage headset presentation.
// 		// m_pRemoteDevice = new CRemoteDevice();
// 		// if (!m_pRemoteDevice->Initialize(
// 		// 		nDisplayX, nDisplayY, nDisplayWidth, nDisplayHeight,
// 		// 		nDisplayRefreshRateNumerator, nDisplayRefreshRateDenominator))
// 		// {
// 		// 	return;
// 		// }

// 		// Spin up a separate thread to handle the overlapped encoding/transmit step.
// 		// m_pEncoder = new CEncoder(m_pD3DRender, m_pRemoteDevice);
// 		// m_pEncoder->Start();
// 	}

// 	virtual ~CDisplayRedirectLatest()
// 	{
// 		// if (m_pEncoder)
// 		// {
// 		// 	m_pEncoder->Stop();
// 		// 	delete m_pEncoder;
// 		// }

// 		// if (m_pRemoteDevice)
// 		// {
// 		// 	m_pRemoteDevice->Shutdown();
// 		// 	delete m_pRemoteDevice;
// 		// }

// 		// if (m_pD3DRender)
// 		// {
// 		// 	m_pD3DRender->Shutdown();
// 		// 	delete m_pD3DRender;
// 		// }
// 	}

// 	bool IsValid() const
// 	{
// 		// return m_pEncoder != NULL;
// 		return false; // lol its always invalid rn should probably fix
// 	}

// 	// ITrackedDeviceServerDriver

// 	virtual vr::EVRInitError Activate(uint32_t unObjectId) override
// 	{
// 		m_unObjectId = unObjectId;

// 		vr::PropertyContainerHandle_t ulContainer =
// 			vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

// 		vr::VRProperties()->SetStringProperty(ulContainer,
// 											  vr::Prop_ModelNumber_String, m_rchModelNumber);
// 		vr::VRProperties()->SetFloatProperty(ulContainer,
// 											 vr::Prop_SecondsFromVsyncToPhotons_Float, m_flAdditionalLatencyInSeconds);
// 		vr::VRProperties()->SetUint64Property(ulContainer,
// 											  vr::Prop_GraphicsAdapterLuid_Uint64, m_nGraphicsAdapterLuid);

// 		return vr::VRInitError_None;
// 	}

// 	virtual void Deactivate() override
// 	{
// 		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
// 	}

// 	virtual void *GetComponent(const char *pchComponentNameAndVersion) override
// 	{
// 		if (!_stricmp(pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version))
// 		{
// 			return static_cast<vr::IVRVirtualDisplay *>(this);
// 		}
// 		return NULL;
// 	}

// private:

// 	// CD3DRender *m_pD3DRender;
// 	// CRemoteDevice *m_pRemoteDevice;
// 	// CEncoder *m_pEncoder;
// };

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
class CServerDriver_Sample : public IServerTrackedDeviceProvider, public IVRVirtualDisplay, public ITrackedDeviceServerDriver
{
public:
	virtual EVRInitError Init(vr::IVRDriverContext *pDriverContext);
	virtual void Cleanup();
	virtual const char *const *GetInterfaceVersions() { return vr::k_InterfaceVersions; }
	virtual void RunFrame();
	virtual bool ShouldBlockStandbyMode() { return false; }
	virtual void EnterStandby() {}
	virtual void LeaveStandby() {}
	virtual void *GetComponent(const char *pchComponentNameAndVersion) override
	{
		DriverLog("CServerDriver_Sample#GetComponent(%s)\n", pchComponentNameAndVersion);

		if (!_stricmp(pchComponentNameAndVersion, vr::IVRVirtualDisplay_Version))
		{
			return static_cast<vr::IVRVirtualDisplay *>(this);
		}
		return NULL;
	}
	virtual vr::EVRInitError Activate(uint32_t unObjectId) override
	{
		m_unObjectId = unObjectId;

		vr::PropertyContainerHandle_t ulContainer =
			vr::VRProperties()->TrackedDeviceToPropertyContainer(unObjectId);

		vr::VRProperties()->SetStringProperty(ulContainer,
											  vr::Prop_ModelNumber_String, m_rchModelNumber);
		vr::VRProperties()->SetFloatProperty(ulContainer,
											 vr::Prop_SecondsFromVsyncToPhotons_Float, m_flAdditionalLatencyInSeconds);
		vr::VRProperties()->SetUint64Property(ulContainer,
											  vr::Prop_GraphicsAdapterLuid_Uint64, m_nGraphicsAdapterLuid);

		return vr::VRInitError_None;
	}

	virtual void Deactivate() override
	{
		m_unObjectId = vr::k_unTrackedDeviceIndexInvalid;
	}
	virtual void DebugRequest(const char *pchRequest, char *pchResponseBuffer, uint32_t unResponseBufferSize) override
	{
		if (unResponseBufferSize >= 1)
			pchResponseBuffer[0] = 0;
	}

	virtual vr::DriverPose_t GetPose() override
	{
		vr::DriverPose_t pose = {0};
		pose.poseIsValid = true;
		pose.result = vr::TrackingResult_Running_OK;
		pose.deviceIsConnected = true;
		pose.qWorldFromDriverRotation.w = 1;
		pose.qWorldFromDriverRotation.x = 0;
		pose.qWorldFromDriverRotation.y = 0;
		pose.qWorldFromDriverRotation.z = 0;
		pose.qDriverFromHeadRotation.w = 1;
		pose.qDriverFromHeadRotation.x = 0;
		pose.qDriverFromHeadRotation.y = 0;
		pose.qDriverFromHeadRotation.z = 0;
		return pose;
	}
	// IVRVirtualDisplay

	virtual void Present(const PresentInfo_t *pPresentInfo, uint32_t unPresentInfoSize) override
	{
		// *pPresentInfo-
		// Open and cache our shared textures to avoid re-opening every frame.
		// ID3D11Texture2D *pTexture = m_pD3DRender->GetSharedTexture((HANDLE)backbufferTextureHandle);
		// if (pTexture == NULL)
		// {
		// 	DriverLog("[VDispDvr] Texture is NULL!");
		// }
		// else
		// {
		DriverLog("[VDispDvr] we be in Present()");
		DriverLog("samplebbth %d || size=%d\n", pPresentInfo->backbufferTextureHandle, unPresentInfoSize);
		// Wait for the encoder to be ready.  This is important because the encoder thread
		// blocks on transmit which uses our shared d3d context (which is not thread safe).
		// m_pEncoder->WaitForEncode();

		// DriverLog("[VDispDvr] Done");

		// Access to shared texture must be wrapped in AcquireSync/ReleaseSync
		// to ensure the compositor has finished rendering to it before it gets used.
		// This enforces scheduling of work on the gpu between processes.
		// IDXGIKeyedMutex *pKeyedMutex = NULL;
		// if (SUCCEEDED(pTexture->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&pKeyedMutex)))
		// {
		// 	if (pKeyedMutex->AcquireSync(0, 10) != S_OK)
		// 	{
		// 		pKeyedMutex->Release();
		// 		DriverLog("[VDispDvr] ACQUIRESYNC FAILED!!!");
		// 		return;
		// 	}
		// }

		// DriverLog("[VDispDvr] AcquiredSync");

		// if (m_pFlushTexture == NULL)
		// {
		// 	D3D11_TEXTURE2D_DESC srcDesc;
		// 	pTexture->GetDesc(&srcDesc);

		// 	// Create a second small texture for copying and reading a single pixel from
		// 	// in order to block on the cpu until rendering is finished.
		// 	D3D11_TEXTURE2D_DESC flushTextureDesc;
		// 	ZeroMemory(&flushTextureDesc, sizeof(flushTextureDesc));
		// 	flushTextureDesc.Width = 32;
		// 	flushTextureDesc.Height = 32;
		// 	flushTextureDesc.MipLevels = 1;
		// 	flushTextureDesc.ArraySize = 1;
		// 	flushTextureDesc.Format = srcDesc.Format;
		// 	flushTextureDesc.SampleDesc.Count = 1;
		// 	flushTextureDesc.Usage = D3D11_USAGE_STAGING;
		// 	flushTextureDesc.BindFlags = 0;
		// 	flushTextureDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

		// 	if (FAILED(m_pD3DRender->GetDevice()->CreateTexture2D(&flushTextureDesc, NULL, &m_pFlushTexture)))
		// 	{
		// 		DriverLog("Failed to create flush texture!");
		// 		return;
		// 	}
		// }

		// // Copy a single pixel so we can block until rendering is finished in WaitForPresent.
		// D3D11_BOX box = {0, 0, 0, 1, 1, 1};
		// m_pD3DRender->GetContext()->CopySubresourceRegion(m_pFlushTexture, 0, 0, 0, 0, pTexture, 0, &box);

		// DriverLog("[VDispDvr] Flush-Begin");

		// // This can go away, but is useful to see it as a separate packet on the gpu in traces.
		// m_pD3DRender->GetContext()->Flush();

		// DriverLog("[VDispDvr] Flush-End");

		// // Copy entire texture to staging so we can read the pixels to send to remote device.
		// m_pEncoder->CopyToStaging(pTexture);

		// DriverLog("[VDispDvr] Flush-Staging(begin)");

		// m_pD3DRender->GetContext()->Flush();

		// DriverLog("[VDispDvr] Flush-Staging(end)");

		// if (pKeyedMutex)
		// {
		// 	pKeyedMutex->ReleaseSync(0);
		// 	pKeyedMutex->Release();
		// }

		// DriverLog("[VDispDvr] ReleasedSync");
		// }
	}

	virtual void WaitForPresent() override
	{
		DriverLog("[VDispDvr] WaitForPresent(begin)");

		// // First wait for rendering to finish on the gpu.
		// // if (m_pFlushTexture)
		// // {
		// // 	D3D11_MAPPED_SUBRESOURCE mapped = {0};
		// // 	if (SUCCEEDED(m_pD3DRender->GetContext()->Map(m_pFlushTexture, 0, D3D11_MAP_READ, 0, &mapped)))
		// // 	{
		// // 		DriverLog("[VDispDvr] Mapped FlushTexture");

		// // 		m_pD3DRender->GetContext()->Unmap(m_pFlushTexture, 0);
		// // 	}
		// // }

		// DriverLog("[VDispDvr] RenderingFinished");

		// // Now that we know rendering is done, we can fire off our thread that reads the
		// // backbuffer into system memory.  We also pass in the earliest time that this frame
		// // should get presented.  This is the real vsync that starts our frame.
		// // m_pEncoder->NewFrameReady(m_flLastVsyncTimeInSeconds + m_flAdditionalLatencyInSeconds);

		// // Get latest timing info to work with.  This gets us sync'd up with the hardware in
		// // the first place, and also avoids any drifting over time.
		// double flLastVsyncTimeInSeconds;
		// uint32_t nVsyncCounter;
		// // m_pRemoteDevice->GetTimingInfo(&flLastVsyncTimeInSeconds, &nVsyncCounter);

		// // Account for encoder/transmit latency.
		// // This is where the conversion from real to virtual vsync happens.
		// flLastVsyncTimeInSeconds -= m_flAdditionalLatencyInSeconds;

		// float flFrameIntervalInSeconds = 5/1000; //m_pRemoteDevice->GetFrameIntervalInSeconds(); -- just a random value i pulled out of my ass for now

		// // Realign our last time interval given updated timing reference.
		// int32_t nTimeRefToLastVsyncFrames =
		// 	(int32_t)roundf(float(m_flLastVsyncTimeInSeconds - flLastVsyncTimeInSeconds) / flFrameIntervalInSeconds);
		// m_flLastVsyncTimeInSeconds = flLastVsyncTimeInSeconds + flFrameIntervalInSeconds * nTimeRefToLastVsyncFrames;

		// // We could probably just use this instead, but it seems safer to go off the system timer calculation.
		// assert(m_nVsyncCounter == nVsyncCounter + nTimeRefToLastVsyncFrames);

		// // double flNow = SystemTime::GetInSeconds();
		// double flNow = 0;

		// // Find the next frame interval (keeping in mind we may get here during running start).
		// int32_t nLastVsyncToNextVsyncFrames =
		// 	(int32_t)(float(flNow - m_flLastVsyncTimeInSeconds) / flFrameIntervalInSeconds);
		// nLastVsyncToNextVsyncFrames = std::max(nLastVsyncToNextVsyncFrames, 0) + 1;

		// And store it for use in GetTimeSinceLastVsync (below) and updating our next frame.
		// m_flLastVsyncTimeInSeconds += flFrameIntervalInSeconds * nLastVsyncToNextVsyncFrames;
		// m_nVsyncCounter = nVsyncCounter + nTimeRefToLastVsyncFrames + nLastVsyncToNextVsyncFrames;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		DriverLog("[VDispDvr] WaitForPresent(end)");
	}

	virtual bool GetTimeSinceLastVsync(float *pfSecondsSinceLastVsync, uint64_t *pulFrameCounter) override
	{
		*pfSecondsSinceLastVsync = (float)(/*SystemTime::GetInSeconds()*/ 0 - m_flLastVsyncTimeInSeconds);
		*pulFrameCounter = m_nVsyncCounter;
		return true;
	}

private:
	CSampleDeviceDriver *m_pNullHmdLatest = nullptr;
	uint32_t m_unObjectId;
	char m_rchSerialNumber[1024];
	char m_rchModelNumber[1024];
	uint64_t m_nGraphicsAdapterLuid;
	float m_flAdditionalLatencyInSeconds;
	double m_flLastVsyncTimeInSeconds;
	uint32_t m_nVsyncCounter;

	// CSampleControllerDriver *m_pController = nullptr;
};

CServerDriver_Sample g_serverDriverNull;

EVRInitError CServerDriver_Sample::Init(vr::IVRDriverContext *pDriverContext)
{
	VR_INIT_SERVER_DRIVER_CONTEXT(pDriverContext);
	InitDriverLog(vr::VRDriverLog());

	m_pNullHmdLatest = new CSampleDeviceDriver();
	vr::VRServerDriverHost()->TrackedDeviceAdded(m_pNullHmdLatest->GetSerialNumber().c_str(), vr::TrackedDeviceClass_HMD, m_pNullHmdLatest);
	// m_pDisplayRedirectLatest = new CDisplayRedirectLatest();
	// vr::VRServerDriverHost()->TrackedDeviceAdded(m_pDisplayRedirectLatest->GetSerialNumber().c_str(), vr::TrackedDeviceClass_DisplayRedirect, m_pDisplayRedirectLatest);
	// m_pController = new CSampleControllerDriver();
	// vr::VRServerDriverHost()->TrackedDeviceAdded(m_pController->GetSerialNumber().c_str(), vr::TrackedDeviceClass_Controller, m_pController);

	return VRInitError_None;
}

void CServerDriver_Sample::Cleanup()
{
	CleanupDriverLog();
	// delete m_pDisplayRedirectLatest;
	// m_pDisplayRedirectLatest = NULL;
	// delete m_pController;
	// m_pController = NULL;
}

void CServerDriver_Sample::RunFrame()
{
	if (m_pNullHmdLatest)
	{
		m_pNullHmdLatest->RunFrame();
	}
	// if (m_pController)
	// {
	// 	m_pController->RunFrame();
	// }

	// vr::VREvent_t vrEvent;
	// while (vr::VRServerDriverHost()->PollNextEvent(&vrEvent, sizeof(vrEvent)))
	// {
	// 	if (m_pController)
	// 	{
	// 		m_pController->ProcessEvent(vrEvent);
	// 	}
	// }
}

//-----------------------------------------------------------------------------
// Purpose:
//-----------------------------------------------------------------------------
HMD_DLL_EXPORT void *HmdDriverFactory(const char *pInterfaceName, int *pReturnCode)
{
	if (0 == strcmp(IServerTrackedDeviceProvider_Version, pInterfaceName))
	{
		return &g_serverDriverNull;
	}
	if (0 == strcmp(IVRWatchdogProvider_Version, pInterfaceName))
	{
		return &g_watchdogDriverNull;
	}

	if (pReturnCode)
		*pReturnCode = VRInitError_Init_InterfaceNotFound;

	return NULL;
}
