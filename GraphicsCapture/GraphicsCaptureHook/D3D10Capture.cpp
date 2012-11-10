/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "GraphicsCaptureHook.h"

#include <D3D10.h>
#include "DXGIStuff.h"


HookData                gi1swapResizeBuffers;
HookData                gi1swapPresent;
FARPROC                 oldD3D10Release = NULL;
FARPROC                 newD3D10Release = NULL;

#define                 NUM_BUFFERS 3
#define                 NULL_ARRAY {NULL, NULL, NULL}

CaptureInfo             d3d10CaptureInfo;

extern SharedTexData    *texData;
extern MemoryCopyData   *copyData;
extern DWORD            copyWait;
extern LONGLONG         lastTime;

LPVOID                  lpCurrentSwap;
extern LPVOID           lpCurrentDevice;
extern MemoryCopyData   *copyData;
extern LPBYTE           textureBuffers[2];
extern DWORD            curCapture;
extern BOOL             bHasTextures;
extern LONGLONG         frameTime;
extern DWORD            fps;
BOOL                    bIsMultisampled = FALSE;

DXGI_FORMAT             dxgiFormat;
ID3D10Texture2D         *d3d10Textures[NUM_BUFFERS] = NULL_ARRAY;
ID3D10Texture2D         *copyD3D10Textures[NUM_BUFFERS] = NULL_ARRAY;


void ClearD3D10Data()
{
    bHasTextures = false;
    for(UINT i=0; i<NUM_BUFFERS; i++)
    {
        SafeRelease(d3d10Textures[i]);
        SafeRelease(copyD3D10Textures[i]);
    }

    DestroySharedMemory();
    copyData = NULL;
    copyWait = 0;
    lastTime = 0;
    fps = 0;
    frameTime = 0;
}

void SetupD3D10(IDXGISwapChain *swapChain)
{
    ClearD3D10Data();

    DXGI_SWAP_CHAIN_DESC scd;
    if(SUCCEEDED(swapChain->GetDesc(&scd)))
    {
        d3d10CaptureInfo.format = ConvertGIBackBufferFormat(scd.BufferDesc.Format);
        if(d3d10CaptureInfo.format != GS_UNKNOWNFORMAT)
        {
            if( dxgiFormat         != scd.BufferDesc.Format ||
                d3d10CaptureInfo.cx != scd.BufferDesc.Width  ||
                d3d10CaptureInfo.cy != scd.BufferDesc.Height )
            {
                dxgiFormat = scd.BufferDesc.Format;
                d3d10CaptureInfo.cx = scd.BufferDesc.Width;
                d3d10CaptureInfo.cy = scd.BufferDesc.Height;
                bIsMultisampled = scd.SampleDesc.Count > 1;
            }
        }
    }

    OSInitializeTimer();
}

struct D3D10Override
{
    UINT STDMETHODCALLTYPE DeviceReleaseHook()
    {
        ID3D10Device *device = (ID3D10Device*)this;

        device->AddRef();
        ULONG refVal = (*(RELEASEPROC)oldD3D10Release)(device);

        if(bHasTextures)
        {
            if(refVal == 5) //our two textures are holding the reference up, so always clear at 3
            {
                ClearD3D10Data();
                lpCurrentDevice = NULL;
                bTargetAcquired = false;
            }
        }
        else if(refVal == 1)
        {
            lpCurrentDevice = NULL;
            bTargetAcquired = false;
        }

        return (*(RELEASEPROC)oldD3D10Release)(device);
    }

    HRESULT STDMETHODCALLTYPE SwapResizeBuffersHook(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT giFormat, UINT flags)
    {
        IDXGISwapChain *swap = (IDXGISwapChain*)this;

        gi1swapResizeBuffers.Unhook();
        HRESULT hRes = swap->ResizeBuffers(bufferCount, width, height, giFormat, flags);
        gi1swapResizeBuffers.Rehook();

        if(lpCurrentSwap == NULL && !bTargetAcquired)
        {
            lpCurrentSwap = swap;
            bTargetAcquired = true;
        }

        if(lpCurrentSwap == swap)
            SetupD3D10(swap);

        return hRes;
    }

    HRESULT STDMETHODCALLTYPE SwapPresentHook(UINT syncInterval, UINT flags)
    {
        IDXGISwapChain *swap = (IDXGISwapChain*)this;

        if(lpCurrentSwap == NULL && !bTargetAcquired)
        {
            lpCurrentSwap = swap;
            SetupD3D10(swap);
            bTargetAcquired = true;
        }

        if(lpCurrentSwap == swap)
        {
            ID3D10Device *device = NULL;
            if(SUCCEEDED(swap->GetDevice(__uuidof(ID3D10Device), (void**)&device)))
            {
                if(!lpCurrentDevice)
                {
                    lpCurrentDevice = device;

                    FARPROC curRelease = GetVTable(device, (8/4));
                    if(curRelease != newD3D10Release)
                    {
                        oldD3D10Release = curRelease;
                        newD3D10Release = ConvertClassProcToFarproc((CLASSPROC)&D3D10Override::DeviceReleaseHook);
                        SetVTable(device, (8/4), newD3D10Release);
                    }
                }

                if(!bHasTextures && bCapturing)
                {
                    if(dxgiFormat)
                    {
                        if(!hwndReceiver)
                            hwndReceiver = FindWindow(RECEIVER_WINDOWCLASS, NULL);

                        if(hwndReceiver)
                        {
                            D3D10_TEXTURE2D_DESC texDesc;
                            ZeroMemory(&texDesc, sizeof(texDesc));
                            texDesc.Width  = d3d10CaptureInfo.cx;
                            texDesc.Height = d3d10CaptureInfo.cy;
                            texDesc.MipLevels = 1;
                            texDesc.ArraySize = 1;
                            texDesc.Format = dxgiFormat;
                            texDesc.SampleDesc.Count = 1;
                            texDesc.Usage = D3D10_USAGE_STAGING;
                            texDesc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;

                            bool bSuccess = true;
                            UINT pitch;

                            for(UINT i=0; i<2; i++)
                            {
                                HRESULT hRes;
                                if(FAILED(hRes = device->CreateTexture2D(&texDesc, NULL, &d3d10Textures[i])))
                                {
                                    logOutput << "SwapPresentHook: device->CreateTexture2D " << i << " failed, result = " << (UINT)hRes << endl;
                                    bSuccess = false;
                                    break;
                                }

                                if(i == 0)
                                {
                                    D3D10_MAPPED_TEXTURE2D map;
                                    if(FAILED(hRes = d3d10Textures[i]->Map(0, D3D10_MAP_READ, 0, &map)))
                                    {
                                        logOutput << "SwapPresentHook: d3d10Textures[" << i << "]->Map failed, result = " << (UINT)hRes << endl;
                                        bSuccess = false;
                                        break;
                                    }

                                    pitch = map.RowPitch;
                                    d3d10Textures[i]->Unmap(0);
                                }
                            }

                            if(bSuccess)
                            {
                                d3d10CaptureInfo.mapID = InitializeSharedMemoryCPUCapture(pitch*d3d10CaptureInfo.cy, &d3d10CaptureInfo.mapSize, &copyData, textureBuffers);
                                if(!d3d10CaptureInfo.mapID)
                                {
                                    logOutput << "SwapPresentHook: failed to create shared memory" << endl;
                                    bSuccess = false;
                                }
                            }

                            if(bSuccess)
                            {
                                bHasTextures = true;
                                d3d10CaptureInfo.captureType = CAPTURETYPE_MEMORY;
                                d3d10CaptureInfo.hwndSender = hwndSender;
                                d3d10CaptureInfo.pitch = pitch;
                                d3d10CaptureInfo.bFlip = FALSE;
                                fps = (DWORD)SendMessage(hwndReceiver, RECEIVER_NEWCAPTURE, 0, (LPARAM)&d3d10CaptureInfo);
                                frameTime = 1000000/LONGLONG(fps);

                                logOutput << "SwapPresentHook: initialization succeeded" << endl;
                            }
                            else
                            {
                                ClearD3D10Data();
                            }
                        }
                    }
                }

                if(bHasTextures)
                {
                    if(bCapturing)
                    {
                        LONGLONG timeVal = OSGetTimeMicroseconds();
                        LONGLONG timeElapsed = timeVal-lastTime;

                        if(timeElapsed >= frameTime)
                        {
                            lastTime += frameTime;

                            DWORD nextCapture = (curCapture == NUM_BUFFERS-1) ? 0 : (curCapture+1);

                            ID3D10Texture2D *sourceTexture = copyD3D10Textures[curCapture];
                            ID3D10Resource *backBuffer = NULL;

                            if(SUCCEEDED(swap->GetBuffer(0, IID_ID3D10Resource, (void**)&backBuffer)))
                            {
                                if(bIsMultisampled)
                                    device->ResolveSubresource(sourceTexture, 0, backBuffer, 0, dxgiFormat);
                                else
                                    device->CopyResource(sourceTexture, backBuffer);
                                backBuffer->Release();

                                if(copyWait < (NUM_BUFFERS-1))
                                    copyWait++;
                                else
                                {
                                    ID3D10Texture2D *prevSourceTexture = copyD3D10Textures[nextCapture];
                                    ID3D10Texture2D *targetTexture = d3d10Textures[nextCapture];

                                    D3D10_MAPPED_TEXTURE2D map;
                                    if(SUCCEEDED(targetTexture->Map(0, D3D10_MAP_READ, 0, &map)))
                                    {
                                        LPBYTE *pTextureBuffer = NULL;
                                        int lastRendered = -1;

                                        //under no circumstances do we -ever- allow a stall
                                        if(WaitForSingleObject(textureMutexes[curCapture], 0) == WAIT_OBJECT_0)
                                            lastRendered = (int)curCapture;
                                        else if(WaitForSingleObject(textureMutexes[nextCapture], 0) == WAIT_OBJECT_0)
                                            lastRendered = (int)nextCapture;

                                        if(lastRendered != -1)
                                        {
                                            SSECopy(textureBuffers[lastRendered], map.pData, map.RowPitch*d3d10CaptureInfo.cy);
                                            ReleaseMutex(textureMutexes[lastRendered]);
                                        }

                                        targetTexture->Unmap(0);
                                        copyData->lastRendered = (UINT)lastRendered;
                                    }

                                    device->CopyResource(targetTexture, prevSourceTexture);
                                }
                            }

                            curCapture = nextCapture;
                        }
                    }
                    else
                        ClearD3D10Data();
                }
            }

            device->Release();
        }

        gi1swapPresent.Unhook();
        HRESULT hRes = swap->Present(syncInterval, flags);
        gi1swapPresent.Rehook();

        return hRes;
    }
};

typedef HRESULT (WINAPI*D3D10CREATEPROC)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D10Device**);

bool InitD3D10Capture()
{
    bool bSuccess = false;

    HMODULE hD3D10Dll = GetModuleHandle(TEXT("d3d10.dll"));
    if(hD3D10Dll)
    {
        D3D10CREATEPROC d3d10Create = (D3D10CREATEPROC)GetProcAddress(hD3D10Dll, "D3D10CreateDeviceAndSwapChain");
        if(d3d10Create)
        {
            DXGI_SWAP_CHAIN_DESC swapDesc;
            ZeroMemory(&swapDesc, sizeof(swapDesc));
            swapDesc.BufferCount = 2;
            swapDesc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            swapDesc.BufferDesc.Width  = 2;
            swapDesc.BufferDesc.Height = 2;
            swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            swapDesc.OutputWindow = hwndSender;
            swapDesc.SampleDesc.Count = 1;
            swapDesc.Windowed = TRUE;

            IDXGISwapChain *swap;
            ID3D10Device *device;

            HRESULT hErr;
            if(SUCCEEDED(hErr = (*d3d10Create)(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &swapDesc, &swap, &device)))
            {
                bSuccess = true;

                UPARAM *vtable = *(UPARAM**)swap;
                gi1swapPresent.Hook((FARPROC)*(vtable+(32/4)), ConvertClassProcToFarproc((CLASSPROC)&D3D10Override::SwapPresentHook));
                gi1swapResizeBuffers.Hook((FARPROC)*(vtable+(52/4)), ConvertClassProcToFarproc((CLASSPROC)&D3D10Override::SwapResizeBuffersHook));

                SafeRelease(swap);
                SafeRelease(device);

                gi1swapPresent.Rehook();
                gi1swapResizeBuffers.Rehook();
            }
            else
                logOutput << "InitD3D10Capture: D3D10CreateDeviceAndSwapChain failed, result = " << hErr << endl;
        }
        else
            logOutput << "InitD3D10Capture: could not get address of D3D10CreateDeviceAndSwapChain" << endl;
    }

    return bSuccess;
}

void FreeD3D10Capture()
{
    gi1swapPresent.Unhook();
    gi1swapResizeBuffers.Unhook();
}
