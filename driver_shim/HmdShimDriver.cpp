// MIT License
//
// Copyright(c) 2025 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "pch.h"

#include "ShimDriverManager.h"
#include "DetourUtils.h"
#include "Tracing.h"

namespace {
    using namespace driver_shim;

    struct DistortionModel {
        float codX;
        float codY;
        float k1;
        float k2;
        float k3;
    };

    // The HmdShimDriver driver wraps another ITrackedDeviceServerDriver instance with the intent to override
    // properties and behaviors.
    struct HmdShimDriver : public vr::ITrackedDeviceServerDriver, vr::IVRDisplayComponent {
        HmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDevice, vr::IVRServerDriverHost* driverHost)
            : m_shimmedDevice(shimmedDevice), m_driverHost(driverHost) {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Ctor");

            TraceLoggingWriteStop(local, "HmdShimDriver_Ctor");
        }

        vr::EVRInitError Activate(uint32_t unObjectId) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Activate", TLArg(unObjectId, "ObjectId"));

            m_deviceIndex = unObjectId;

            const vr::PropertyContainerHandle_t container =
                vr::VRProperties()->TrackedDeviceToPropertyContainer(m_deviceIndex);

            // Activate the real device driver.
            const auto status = m_shimmedDevice->Activate(unObjectId);

            // Acquire the IVRDisplayComponent.
            m_shimmedDisplayComponent =
                (vr::IVRDisplayComponent*)m_shimmedDevice->GetComponent(vr::IVRDisplayComponent_Version);
            if (m_shimmedDisplayComponent) {
                // Enable our settings menu.
                vr::VRProperties()->SetStringProperty(container, vr::Prop_ResourceRoot_String, "distortion_shim");
                vr::VRProperties()->SetStringProperty(container,
                                                      vr::Prop_AdditionalDeviceSettingsPath_String,
                                                      "{distortion_shim}/settings/settingsschema.vrsettings");

                // FIXME: Here you can change some properties related to distortion, for example set resolution of the
                // distortion mesh.
                // vr::VRProperties()->SetInt32Property(container, vr::Prop_DistortionMeshResolution_Int32, 64);

                // Populate our distortion parameters from the config.
                ReadDistortionModel();

                // FIXME: You will also want to modify or disable the hidden area mesh based on the lens geometry.
                // Here we disable it.
                vr::CVRHiddenAreaHelpers helpers{vr::VRPropertiesRaw()};
                helpers.SetHiddenArea(vr::Eye_Left, vr::k_eHiddenAreaMesh_Standard, nullptr, 0);
                helpers.SetHiddenArea(vr::Eye_Left, vr::k_eHiddenAreaMesh_Inverse, nullptr, 0);
                helpers.SetHiddenArea(vr::Eye_Right, vr::k_eHiddenAreaMesh_Standard, nullptr, 0);
                helpers.SetHiddenArea(vr::Eye_Right, vr::k_eHiddenAreaMesh_Inverse, nullptr, 0);
            }

            TraceLoggingWriteStop(local, "HmdShimDriver_Activate");

            return status;
        }

        void Deactivate() override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdShimDriver_Deactivate", TLArg(m_deviceIndex, "ObjectId"));

            m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;

            m_shimmedDevice->Deactivate();

            DriverLog("Deactivated device shimmed with HmdShimDriver");

            TraceLoggingWriteStop(local, "HmdShimDriver_Deactivate");
        }

        void EnterStandby() override {
            m_shimmedDevice->EnterStandby();
        }

        void* GetComponent(const char* pchComponentNameAndVersion) override {
            void* component = m_shimmedDevice->GetComponent(pchComponentNameAndVersion);
            DriverLog("GetComponent(%s) = %p", pchComponentNameAndVersion, component);
            if (component) {
                const std::string_view componentNameAndVersion(pchComponentNameAndVersion);
                if (componentNameAndVersion == vr::IVRDisplayComponent_Version) {
                    m_shimmedDisplayComponent = (vr::IVRDisplayComponent*)component;
                    component = (vr::IVRDisplayComponent*)this;
                } else if (componentNameAndVersion == vr::IVRDriverDirectModeComponent_Version) {
                    // A driver with a "direct mode component" is not a SteamVR native direct mode driver.
                    m_isNotDirectModeDriver = true;
                } else if (componentNameAndVersion == vr::IVRVirtualDisplay_Version) {
                    // A driver with a "virtual display" is not a SteamVR native direct mode driver.
                    m_isNotDirectModeDriver = true;
                }
            }
            return component;
        }

        vr::DriverPose_t GetPose() override {
            return m_shimmedDevice->GetPose();
        }

        void DebugRequest(const char* pchRequest, char* pchResponseBuffer, uint32_t unResponseBufferSize) override {
            m_shimmedDevice->DebugRequest(pchRequest, pchResponseBuffer, unResponseBufferSize);
        }

        void GetWindowBounds(int32_t* pnX, int32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
            // Not used by drivers in direct mode.
            // Forward the call for other drivers.
            m_shimmedDisplayComponent->GetWindowBounds(pnX, pnY, pnWidth, pnHeight);
        }

        bool IsDisplayOnDesktop() override {
            // Should always be false for drivers in direct mode.
            // Forward the call for other drivers.
            return m_shimmedDisplayComponent->IsDisplayOnDesktop();
        }

        bool IsDisplayRealDisplay() override {
            // Should always be true for drivers in direct mode.
            // Forward the call for other drivers.
            return m_shimmedDisplayComponent->IsDisplayRealDisplay();
        }

        void GetRecommendedRenderTargetSize(uint32_t* pnWidth, uint32_t* pnHeight) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_GetRecommendedRenderTargetSize", TLArg(m_deviceIndex, "ObjectId"));

            if (m_isNotDirectModeDriver) {
                // Forward as-is for drivers not in direct mode.
                m_shimmedDisplayComponent->GetRecommendedRenderTargetSize(pnWidth, pnHeight);
            } else {
                // FIXME: Changing the distortion may require to adjust the resolution to match the desired pixel
                // density post-distortion.
                // Here we just leave-is for the example.
                m_shimmedDisplayComponent->GetRecommendedRenderTargetSize(pnWidth, pnHeight);
            }

            TraceLoggingWriteStop(local,
                                  "HmdDriver_GetRecommendedRenderTargetSize",
                                  TLArg(*pnWidth, "RecommendedWidth"),
                                  TLArg(*pnHeight, "RecommendedHeight"));
        }

        void GetEyeOutputViewport(
            vr::EVREye eEye, uint32_t* pnX, uint32_t* pnY, uint32_t* pnWidth, uint32_t* pnHeight) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_GetEyeOutputViewport",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(eEye == vr::Eye_Left ? "Left" : "Right", "Eye"));

            // Changing the distortion would typically not change the viewport of each eye, so we forward the call
            // as-is.
            m_shimmedDisplayComponent->GetEyeOutputViewport(eEye, pnX, pnY, pnWidth, pnHeight);

            TraceLoggingWriteStop(local,
                                  "HmdDriver_GetEyeOutputViewport",
                                  TLArg(*pnX, "X"),
                                  TLArg(*pnY, "Y"),
                                  TLArg(*pnWidth, "Width"),
                                  TLArg(*pnHeight, "Height"));
        }

        void GetProjectionRaw(vr::EVREye eEye, float* pfLeft, float* pfRight, float* pfTop, float* pfBottom) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_GetProjectionRaw",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(eEye == vr::Eye_Left ? "Left" : "Right", "Eye"));

            if (m_isNotDirectModeDriver) {
                // Forward as-is for drivers not in direct mode.
                m_shimmedDisplayComponent->GetProjectionRaw(eEye, pfLeft, pfRight, pfTop, pfBottom);
            } else {
                // FIXME: Changing the distortion may require to adjust the FOV to match the new lens geometry.
                // Here we just leave-is for the example.
                m_shimmedDisplayComponent->GetProjectionRaw(eEye, pfLeft, pfRight, pfTop, pfBottom);
            }

            TraceLoggingWriteStop(local,
                                  "HmdDriver_GetProjectionRaw",
                                  TLArg(*pfLeft, "Left"),
                                  TLArg(*pfRight, "Right"),
                                  TLArg(*pfBottom, "Bottom"),
                                  TLArg(*pfTop, "Top"));
        }

        vr::DistortionCoordinates_t ComputeDistortion(vr::EVREye eEye, float fU, float fV) override {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local,
                                   "HmdDriver_ComputeDistortion",
                                   TLArg(m_deviceIndex, "ObjectId"),
                                   TLArg(eEye == vr::Eye_Left ? "Left" : "Right", "Eye"),
                                   TLArg(fU, "U"),
                                   TLArg(fV, "V"));

            vr::DistortionCoordinates_t result{};
            if (m_isNotDirectModeDriver) {
                // Forward as-is for drivers not in direct mode (should not be used anyway...).
                result = m_shimmedDisplayComponent->ComputeDistortion(eEye, fU, fV);
            } else {
                // FIXME: This is where you change the distortion function!
                // Here's an example using Brown-Conrady with some dummy parameters.

                // Transform input coordinates to pixels.
                uint32_t dummy, width, height;
                m_shimmedDisplayComponent->GetEyeOutputViewport(eEye, &dummy, &dummy, &width, &height);
                const float x = fU * width;
                const float y = fV * height;

                // Brown-Conrady function itself.
                const auto BrownConrady = [](float x,
                                             float y,
                                             DirectX::XMMATRIX invAffine,
                                             float codX,
                                             float codY,
                                             float k1,
                                             float k2,
                                             float k3) -> DirectX::XMFLOAT2 {
                    using namespace DirectX;

                    // Apply radial and tangential distortion.
                    const XMFLOAT2 delta(x - codX, y - codY);
                    const float r2 = delta.x * delta.x + delta.y * delta.y;
                    const float d = 1.0f + r2 * (k1 + r2 * (k2 + r2 * k3));
                    const XMVECTOR p = XMVectorSet((delta.x * d) + codX, (delta.y * d) + codY, 1.f, 1.f);

                    // Correct projection.
                    XMVECTOR vp = XMVector3Transform(p, invAffine);
                    vp /= XMVectorGetW(vp);

                    return XMFLOAT2(XMVectorGetX(vp), XMVectorGetY(vp));
                };

                // Transform final coordinates based on tangents.
                float fLeft, fRight, fTop, fBottom;
                m_shimmedDisplayComponent->GetProjectionRaw(eEye, &fLeft, &fRight, &fBottom, &fTop);
                fLeft = std::abs(fLeft);
                fRight = std::abs(fRight);
                fTop = std::abs(fTop);
                fBottom = std::abs(fBottom);
                const float horizontalAperture = (fLeft + fRight);
                const float verticalAperture = (fTop + fBottom);
                const auto SetResult = [&](float* result, const DirectX::XMFLOAT2& uv) {
                    result[0] = (uv.x + fLeft) / horizontalAperture;
                    result[1] = (uv.y + fTop) / verticalAperture;
                };

                // Apply the distortion to each channel.
                SetResult(result.rfRed,
                          BrownConrady(x,
                                       y,
                                       m_invAffine[eEye],
                                       m_distortionModel[eEye][0].codX,
                                       m_distortionModel[eEye][0].codY,
                                       m_distortionModel[eEye][0].k1,
                                       m_distortionModel[eEye][0].k2,
                                       m_distortionModel[eEye][0].k3));
                SetResult(result.rfGreen,
                          BrownConrady(x,
                                       y,
                                       m_invAffine[eEye],
                                       m_distortionModel[eEye][1].codX,
                                       m_distortionModel[eEye][1].codY,
                                       m_distortionModel[eEye][1].k1,
                                       m_distortionModel[eEye][1].k2,
                                       m_distortionModel[eEye][1].k3));
                SetResult(result.rfBlue,
                          BrownConrady(x,
                                       y,
                                       m_invAffine[eEye],
                                       m_distortionModel[eEye][2].codX,
                                       m_distortionModel[eEye][2].codY,
                                       m_distortionModel[eEye][2].k1,
                                       m_distortionModel[eEye][2].k2,
                                       m_distortionModel[eEye][2].k3));
            }

            TraceLoggingWriteStop(local,
                                  "HmdDriver_ComputeDistortion",
                                  TLArg(result.rfRed[0], "RedX"),
                                  TLArg(result.rfRed[1], "RedY"),
                                  TLArg(result.rfGreen[0], "GreenX"),
                                  TLArg(result.rfGreen[1], "GreenY"),
                                  TLArg(result.rfBlue[0], "BlueX"),
                                  TLArg(result.rfBlue[1], "BlueY"));

            return result;
        }

        bool ComputeInverseDistortion(
            vr::HmdVector2_t* pResult, vr::EVREye eEye, uint32_t unChannel, float fU, float fV) override {
            // Typically not supported, but we forward the call anyway.
            return m_shimmedDisplayComponent->ComputeInverseDistortion(pResult, eEye, unChannel, fU, fV);
        }

        bool ReadDistortionModel() {
            uint32_t dummy, width, height;
            m_shimmedDisplayComponent->GetEyeOutputViewport(vr::Eye_Left, &dummy, &dummy, &width, &height);

            // Retrieve Brown-Conrady parameters for both eyes.
            decltype(m_distortionModel) newDistortionModel;
            newDistortionModel[0][0].codX =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_red_cod_x") * width;
            newDistortionModel[0][0].codY =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_red_cod_y") * height;
            newDistortionModel[0][0].k1 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_red_k1");
            newDistortionModel[0][0].k2 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_red_k2");
            newDistortionModel[0][0].k3 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_red_k3");
            newDistortionModel[0][1].codX =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_green_cod_x") * width;
            newDistortionModel[0][1].codY =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_green_cod_y") * height;
            newDistortionModel[0][1].k1 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_green_k1");
            newDistortionModel[0][1].k2 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_green_k2");
            newDistortionModel[0][1].k3 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_green_k3");
            newDistortionModel[0][2].codX =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_blue_cod_x") * width;
            newDistortionModel[0][2].codY =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_blue_cod_y") * height;
            newDistortionModel[0][2].k1 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_blue_k1");
            newDistortionModel[0][2].k2 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_blue_k2");
            newDistortionModel[0][2].k3 = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_blue_k3");
            newDistortionModel[1][0].codX =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_red_cod_x") * width;
            newDistortionModel[1][0].codY =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_red_cod_y") * height;
            newDistortionModel[1][0].k1 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_red_k1");
            newDistortionModel[1][0].k2 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_red_k2");
            newDistortionModel[1][0].k3 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_red_k3");
            newDistortionModel[1][1].codX =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_green_cod_x") * width;
            newDistortionModel[1][1].codY =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_green_cod_y") * height;
            newDistortionModel[1][1].k1 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_green_k1");
            newDistortionModel[1][1].k2 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_green_k2");
            newDistortionModel[1][1].k3 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_green_k3");
            newDistortionModel[1][2].codX =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_blue_cod_x") * width;
            newDistortionModel[1][2].codY =
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_blue_cod_y") * height;
            newDistortionModel[1][2].k1 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_blue_k1");
            newDistortionModel[1][2].k2 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_blue_k2");
            newDistortionModel[1][2].k3 = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_blue_k3");

            // Retrieve Affine matrix parameters for left eye.
            const DirectX::XMFLOAT2 focalLengthLeft(
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_focal_length_x") * width,
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_focal_length_y") * height);
            const DirectX::XMFLOAT2 principalPointLeft(
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_principal_point_x") * width,
                vr::VRSettings()->GetFloat("driver_distortion_shim", "left_principal_point_y") * height);
            const float skewFactorLeft = vr::VRSettings()->GetFloat("driver_distortion_shim", "left_skew_factor");
            const DirectX::XMMATRIX newAffineLeft(
                // clang-format off
                focalLengthLeft.x,    0.0f,                 0.0f, 0.0f,
                skewFactorLeft,       focalLengthLeft.y,    0.0f, 0.0f,
                principalPointLeft.x, principalPointLeft.y, 1.0f, 0.0f,
                0.0f,                 0.0f,                 0.0f, 1.0f);
            // clang-format on

            // Retrieve Affine matrix parameters for right eye.
            const DirectX::XMFLOAT2 focalLengthRight(
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_focal_length_x") * width,
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_focal_length_y") * height);
            const DirectX::XMFLOAT2 principalPointRight(
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_principal_point_x") * width,
                vr::VRSettings()->GetFloat("driver_distortion_shim", "right_principal_point_y") * height);
            const float skewFactorRight = vr::VRSettings()->GetFloat("driver_distortion_shim", "right_skew_factor");
            const DirectX::XMMATRIX newAffineRight(
                // clang-format off
                focalLengthRight.x,    0.0f,                  0.0f, 0.0f,
                skewFactorRight,       focalLengthRight.y,    0.0f, 0.0f,
                principalPointRight.x, principalPointRight.y, 1.0f, 0.0f,
                0.0f,                  0.0f,                  0.0f, 1.0f);
            // clang-format on

            // Detect changes.
            const bool changed = memcmp(m_distortionModel, newDistortionModel, sizeof(newDistortionModel)) ||
                                 memcmp(&m_affine[0], &newAffineLeft, sizeof(newAffineLeft)) ||
                                 memcmp(&m_affine[1], &newAffineRight, sizeof(newAffineRight));

            // Commit changes.
            memcpy(m_distortionModel, newDistortionModel, sizeof(newDistortionModel));
            m_affine[0] = newAffineLeft;
            m_affine[1] = newAffineRight;
            m_invAffine[0] = DirectX::XMMatrixInverse(nullptr, m_affine[0]);
            m_invAffine[1] = DirectX::XMMatrixInverse(nullptr, m_affine[1]);

            return changed;
        }

        void ApplySettingsChanges() {
            TraceLocalActivity(local);
            TraceLoggingWriteStart(local, "HmdDriver_ApplySettingsChanges", TLArg(m_deviceIndex, "ObjectId"));

            // Don't do anything if your shim did not hook a display driver.
            if (m_shimmedDisplayComponent && !m_isNotDirectModeDriver) {
                const bool distortionChanged = ReadDistortionModel();
                if (distortionChanged) {
                    // Force SteamVR to recompute the distortion mesh (calling ComputeDistortion() etc...)
                    m_driverHost->VendorSpecificEvent(m_deviceIndex, vr::VREvent_LensDistortionChanged, {}, 0.0);

                    // FIXME: You probably want to recompute the hidden area mesh here too.
                    // In our example, we disabled it entirely (see Activate()).
                }
            }

            TraceLoggingWriteStop(local, "HmdDriver_ApplySettingsChanges", );
        }

        vr::ITrackedDeviceServerDriver* const m_shimmedDevice;
        vr::IVRServerDriverHost* const m_driverHost;
        vr::TrackedDeviceIndex_t m_deviceIndex = vr::k_unTrackedDeviceIndexInvalid;
        vr::IVRDisplayComponent* m_shimmedDisplayComponent = nullptr;
        bool m_isNotDirectModeDriver = false;

        // Affine transform.
        DirectX::XMMATRIX m_affine[2];
        DirectX::XMMATRIX m_invAffine[2];

        // Distortion parameters for 2 eyes, 3 channels.
        DistortionModel m_distortionModel[2][3];
    };
} // namespace

namespace driver_shim {

    std::vector<HmdShimDriver*> drivers;

    vr::ITrackedDeviceServerDriver* CreateHmdShimDriver(vr::ITrackedDeviceServerDriver* shimmedDriver,
                                                        vr::IVRServerDriverHost* driverHost) {
        auto driver = new HmdShimDriver(shimmedDriver, driverHost);
        drivers.push_back(driver);
        return driver;
    }

    void ApplySettingsChanges() {
        for (auto driver : drivers) {
            driver->ApplySettingsChanges();
        }
    }

} // namespace driver_shim
