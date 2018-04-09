//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#pragma once

#include "DXSample.h"
#include "StepTimer.h"
#include "RaytracingHlslCompat.h"
#include "Camera.h"
#include "GameInput.h"
#include "CameraController.h"

class Primitive
{
public:
    std::string m_name;
    ComPtr<ID3D12Resource> m_positionBuffer;
    ComPtr<ID3D12Resource> m_normalBuffer;
    ComPtr<ID3D12Resource> m_indexBuffer;

    D3D12_RAYTRACING_GEOMETRY_DESC m_geometryDesc;

    PerMaterialCB m_material;
};

namespace GlobalRootSignatureParams {
    enum Value { 
        OutputViewSlot = 0,
        AccelerationStructureSlot,
        PerFrameCBSlot,
        NormalBuffersSlot,
        IndexBuffersSlot,
        RndSamplesBufferSlot,
        Count 
    };
}

namespace LocalRootSignatureParams {
    enum Value {
        ViewportConstantSlot = 0,
        Count 
    };
}

// The sample supports both Raytracing Fallback Layer and DirectX Raytracing APIs. 
// This is purely for demonstration purposes to show where the API differences are. 
// Real-world applications will implement only one or the other. 
// Fallback Layer uses DirectX Raytracing if a driver and OS supports it. 
// Otherwise, it falls back to compute pipeline to emulate raytracing.
// Developers aiming for a wider HW support should target Fallback Layer.
class D3D12RaytracingHelloWorld : public DXSample
{
    enum class RaytracingAPI {
        FallbackLayer,
        DirectXRaytracing,
    };

public:
    D3D12RaytracingHelloWorld(UINT width, UINT height, std::wstring name);

    // IDeviceNotify
    virtual void OnDeviceLost() override;
    virtual void OnDeviceRestored() override;

    // Messages
    virtual void OnInit();
    virtual void OnKeyDown(UINT8 key);
    virtual void OnUpdate();
    virtual void OnRender();
    virtual void OnSizeChanged(UINT width, UINT height, bool minimized);
    virtual void OnDestroy();
    virtual IDXGISwapChain* GetSwapchain() { return m_deviceResources->GetSwapChain(); }

private:

    static const UINT FrameCount = 3;
        
    // Raytracing Fallback Layer (FL) attributes
    ComPtr<ID3D12RaytracingFallbackDevice> m_fallbackDevice;
    ComPtr<ID3D12RaytracingFallbackCommandList> m_fallbackCommandList;
    ComPtr<ID3D12RaytracingFallbackStateObject> m_fallbackStateObject;
    WRAPPED_GPU_POINTER m_fallbackTopLevelAccelerationStructurePointer;

    // DirectX Raytracing (DXR) attributes
    ComPtr<ID3D12DeviceRaytracingPrototype> m_dxrDevice;
    ComPtr<ID3D12CommandListRaytracingPrototype> m_dxrCommandList;
    ComPtr<ID3D12StateObjectPrototype> m_dxrStateObject;
    bool m_isDxrSupported;

    // Root signatures
    ComPtr<ID3D12RootSignature> m_raytracingGlobalRootSignature;
    ComPtr<ID3D12RootSignature> m_raytracingLocalRootSignature;

    // Descriptors
    ComPtr<ID3D12DescriptorHeap> m_descriptorHeap;
    UINT m_descriptorsAllocated;
    UINT m_descriptorSize;
    
    // Raytracing scene
    PerFrameCB m_perFrameCBContent;
    ComPtr<ID3D12Resource> m_perFrameCB[FrameCount];
    uint32_t m_cbIdx = 0;
    float m_iter = 0.0;

    // Geometry
    std::vector<Primitive> m_primitives;

    // Acceleration structure
    ComPtr<ID3D12Resource> m_accelerationStructure;
    ComPtr<ID3D12Resource> m_bottomLevelAccelerationStructure;
    ComPtr<ID3D12Resource> m_topLevelAccelerationStructure;

    // Raytracing output
    ComPtr<ID3D12Resource> m_raytracingOutput;
    D3D12_GPU_DESCRIPTOR_HANDLE m_raytracingOutputResourceUAVGpuDescriptor;
    UINT m_raytracingOutputResourceUAVDescriptorHeapIndex;

    D3D12_GPU_DESCRIPTOR_HANDLE m_normalBuffersSRVGpuDescriptor;
    D3D12_GPU_DESCRIPTOR_HANDLE m_indexBuffersSRVGpuDescriptor;

    // Shader tables
    static const wchar_t* c_hitGroupName;
    static const wchar_t* c_raygenShaderName;
    static const wchar_t* c_closestHitShaderName;
    static const wchar_t* c_missShaderName;
    ComPtr<ID3D12Resource> m_missShaderTable;
    ComPtr<ID3D12Resource> m_rayGenShaderTable;
    ComPtr<ID3D12Resource> m_hitGroupShaderTable;
    
    // Application state
    RaytracingAPI m_raytracingAPI;
    bool m_forceComputeFallback;
    StepTimer m_timer;

    int32_t m_shaderRecordSize = 0;

    Math::Camera m_camera;
    GameCore::CameraController m_cameraController;

    struct RndSamples
    {
        float p0;
        float p1;
    };
    int32_t m_numOfRndSamples = 32*1024;
    ComPtr<ID3D12Resource> m_rndSamples;
    ComPtr<ID3D12Resource> m_rndSamplesCounter;
    ComPtr<ID3D12Resource> m_rndSamplesUploadCopy;
    ComPtr<ID3D12Resource> m_rndSamplesCounterUploadCopy;
    D3D12_GPU_DESCRIPTOR_HANDLE m_rndSamplesUAVGpuDescriptor;
    D3D12_GPU_DESCRIPTOR_HANDLE m_rndSamplesCounterUAVGpuDescriptor;

    int32_t m_numAllRndSamples = 1024*1024*8;
    std::unique_ptr<RndSamples[]> m_allRndSamples;

    void ParseCommandLineArgs(WCHAR* argv[], int argc);
    void RecreateD3D();
    void DoRaytracing();   
    void CreateDeviceDependentResources();
    void CreateWindowSizeDependentResources();
    void ReleaseDeviceDependentResources();
    void ReleaseWindowSizeDependentResources();
    void CreateRaytracingDevice();
    void SerializeAndCreateRaytracingRootSignature(D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rootSig);
    void CreateRootSignatures();
    void CreateRaytracingPipelineStateObject();
    void CreateDescriptorHeap();
    void CreateRaytracingOutputResource();
    void CreateConstantBuffers();
    void CreateRandomSamplesBuffer();
    void BuildGeometry();
    void BuildAccelerationStructures();
    void BuildShaderTables();
    void SelectRaytracingAPI(RaytracingAPI type);
    void UpdateForSizeChange(UINT clientWidth, UINT clientHeight);
    void CopyRaytracingOutputToBackbuffer();
    void CalculateFrameStats();
    UINT AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor, UINT descriptorIndexToUse = UINT_MAX);
    WRAPPED_GPU_POINTER CreateFallbackWrappedPointer(ID3D12Resource* resource, UINT bufferNumElements);
};
