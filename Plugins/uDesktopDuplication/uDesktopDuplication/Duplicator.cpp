#pragma once

#include "Duplicator.h"
#include "Monitor.h"
#include "Device.h"
#include "Common.h"
#include "Debug.h"

#include "IUnityInterface.h"
#include "IUnityGraphicsD3D11.h"

using namespace Microsoft::WRL;



Duplicator::Duplicator(Monitor* monitor)
    : monitor_(monitor)
{
    InitializeDevice();
    InitializeDuplication();
    CheckUnityAdapter();
}


Duplicator::~Duplicator()
{
    Stop();
}


void Duplicator::InitializeDevice()
{
    device_ = std::make_shared<IsolatedD3D11Device>();

    if (FAILED(device_->Create(monitor_->GetAdapter())))
    {
        Debug::Error("Monitor::Initialize() => IsolatedD3D11Device::Create() failed.");
        state_ = State::Unknown;
    }
}


void Duplicator::InitializeDuplication()
{
	ComPtr<IDXGIOutput1> output1;
	if (FAILED(monitor_->GetOutput().As(&output1))) {
		return;
	}

	auto hr = output1->DuplicateOutput(device_->GetDevice().Get(), &dupl_);
	switch (hr)
	{
		case S_OK:
		{
			state_ = State::Ready;
			const auto rot = static_cast<DXGI_MODE_ROTATION>(monitor_->GetRotation());
			Debug::Log("Duplicator::Initialize() => OK.");
			Debug::Log("    ID    : ", monitor_->GetId());
			Debug::Log("    Size  : (", monitor_->GetWidth(), ", ", monitor_->GetHeight(), ")");
			Debug::Log("    DPI   : (", monitor_->GetDpiX(), ", ", monitor_->GetDpiY(), ")");
			Debug::Log("    Rot   : ",
				rot == DXGI_MODE_ROTATION_IDENTITY ? "Landscape" :
				rot == DXGI_MODE_ROTATION_ROTATE90 ? "Portrait" :
				rot == DXGI_MODE_ROTATION_ROTATE180 ? "Landscape (flipped)" :
				rot == DXGI_MODE_ROTATION_ROTATE270 ? "Portrait (flipped)" :
				"Unspecified");
			break;
		}
		case E_INVALIDARG:
		{
			state_ = State::InvalidArg;
			Debug::Error("Duplicator::Initialize() => Invalid arguments.");
			break;
		}
		case E_ACCESSDENIED:
		{
			// For example, when the user presses Ctrl + Alt + Delete and the screen
			// switches to admin screen, this error occurs. 
			state_ = State::AccessDenied;
			Debug::Error("Duplicator::Initialize() => Access denied.");
			break;
		}
		case DXGI_ERROR_UNSUPPORTED:
		{
			// If the display adapter on the computer is running under the Microsoft Hybrid system,
			// this error occurs.
			state_ = State::Unsupported;
			Debug::Error("Duplicator::Initialize() => Unsupported display.");
			break;
		}
		case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE:
		{
			// When other application use Desktop Duplication API, this error occurs.
			state_ = State::CurrentlyNotAvailable;
			Debug::Error("Duplicator::Initialize() => Currently not available.");
			break;
		}
		case DXGI_ERROR_SESSION_DISCONNECTED:
		{
			state_ = State::SessionDisconnected;
			Debug::Error("Duplicator::Initialize() => Session disconnected.");
			break;
		}
		default:
		{
			state_ = State::Unknown;
			Debug::Error("Duplicator::Render() => Unknown Error.");
			break;
		}
	}
}


void Duplicator::CheckUnityAdapter()
{
    DXGI_ADAPTER_DESC desc;
    monitor_->GetAdapter()->GetDesc(&desc);

    const auto unityAdapterLuid = GetUnityAdapterLuid();
    const auto isUnityAdapter =
        (desc.AdapterLuid.LowPart  == unityAdapterLuid.LowPart) &&
        (desc.AdapterLuid.HighPart == unityAdapterLuid.HighPart);

    if (!isUnityAdapter)
    {
        Debug::Error("Duplicator::CheckUnityAdapter() => The adapter is not same as Unity, and now this case is not supported.");
        state_ = State::Unsupported;
    }
}


void Duplicator::Start()
{
    if (state_ != State::Ready) return;

    Stop();

    thread_ = std::thread([this] 
    {
        state_ = State::Running;

        shouldRun_ = true;
        while (shouldRun_)
        {
            if (!Duplicate()) break;
        }

        if (state_ == State::Running)
        {
            state_ = State::Ready;
        }
    });
}


void Duplicator::Stop()
{
    shouldRun_ = false;

    if (thread_.joinable())
    {
        thread_.join();
    }
}


bool Duplicator::IsRunning() const
{
    return state_ == State::Running;
}


bool Duplicator::IsError() const
{
    return
        state_ != State::Ready &&
        state_ != State::Running;
}


Duplicator::State Duplicator::GetState() const
{
    return state_;
}


ComPtr<IDXGIOutputDuplication> Duplicator::GetDuplication()
{
    return dupl_;
}


const Duplicator::Frame& Duplicator::GetFrame() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return lastFrame_;
}


bool Duplicator::Duplicate()
{
    if (!dupl_ || !device_) return false;

    ComPtr<IDXGIResource> resource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    const auto hr = dupl_->AcquireNextFrame(INFINITE, &frameInfo, &resource);

    if (FAILED(hr)) 
    {
        switch (hr)
        {
            case DXGI_ERROR_ACCESS_LOST:
            {
                // If any monitor setting has changed (e.g. monitor size has changed),
                // it is necessary to re-initialize monitors.
                Debug::Log("Monitor::Render() => DXGI_ERROR_ACCESS_LOST.");
                state_ = State::AccessLost;
                break;
            }
            case DXGI_ERROR_WAIT_TIMEOUT:
            {
                // This often occurs when timeout value is small and it is not problem. 
                // Debug::Log("Monitor::Render() => DXGI_ERROR_WAIT_TIMEOUT.");
                break;
            }
            case DXGI_ERROR_INVALID_CALL:
            {
                Debug::Error("Monitor::Render() => DXGI_ERROR_INVALID_CALL.");
                break;
            }
            case E_INVALIDARG:
            {
                Debug::Error("Monitor::Render() => E_INVALIDARG.");
                break;
            }
            default:
            {
                state_ = State::Unknown;
                Debug::Error("Monitor::Render() => Unknown Error.");
                break;
            }
        }
        return false;
    }

    ScopedReleaser releaser([this] 
    {
        const auto hr = dupl_->ReleaseFrame();
        if (FAILED(hr))
        {
            switch (hr)
            {
                case DXGI_ERROR_ACCESS_LOST:
                {
                    Debug::Log("Monitor::Render() => DXGI_ERROR_ACCESS_LOST.");
                    state_ = State::AccessLost;
                    break;
                }
                case DXGI_ERROR_INVALID_CALL:
                {
                    Debug::Error("Monitor::Render() => DXGI_ERROR_INVALID_CALL.");
                    break;
                }
                default:
                {
                    state_ = State::Unknown;
                    Debug::Error("Monitor::Render() => Unknown Error.");
                    break;
                }
            }
        }
    });

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) 
    {
        return false;
    }

    auto copyTarget = device_->GetCompatibleSharedTexture(texture);
    if (!copyTarget)
    {
        return false;
    }

    ComPtr<ID3D11DeviceContext> context;
    device_->GetDevice()->GetImmediateContext(&context);
    context->CopyResource(copyTarget.Get(), texture.Get());

    {
        std::lock_guard<std::mutex> lock(mutex_);
        lastFrame_ = Frame { copyTarget, frameInfo };
    }

    return true;
}