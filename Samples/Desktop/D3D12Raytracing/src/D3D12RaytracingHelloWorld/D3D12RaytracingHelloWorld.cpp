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

#include "stdafx.h"
#include "D3D12RaytracingHelloWorld.h"
#include "DirectXRaytracingHelper.h"
#include "CompiledShaders\Raytracing.hlsl.h"
#define TINYOBJLOADER_IMPLEMENTATION
#include "tiny_obj_loader.h"
#include "Math/Vector.h"
#include <random>

namespace GameCore
{
    extern HWND g_hWnd;
}

using namespace std;
using namespace DX;

const wchar_t* D3D12RaytracingHelloWorld::c_hitGroupName = L"MyHitGroup";
const wchar_t* D3D12RaytracingHelloWorld::c_raygenShaderName = L"MyRaygenShader";
const wchar_t* D3D12RaytracingHelloWorld::c_closestHitShaderName = L"MyClosestHitShader";
const wchar_t* D3D12RaytracingHelloWorld::c_missShaderName = L"MyMissShader";

D3D12RaytracingHelloWorld::D3D12RaytracingHelloWorld(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    m_raytracingOutputResourceUAVDescriptorHeapIndex(UINT_MAX),
    m_cameraController{m_camera, Math::Vector3{0, 1, 0}}
{
    m_isDxrSupported = EnableRaytracing();
    if (!m_isDxrSupported)
    {
        OutputDebugString(
            L"Could not enable raytracing driver (D3D12EnableExperimentalFeatures() failed).\n" \
            L"Possible reasons:\n" \
            L"  1) your OS is not in developer mode.\n" \
            L"  2) your GPU driver doesn't match the D3D12 runtime loaded by the app (d3d12.dll and friends).\n" \
            L"  3) your D3D12 runtime doesn't match the D3D12 headers used by your app (in particular, the GUID passed to D3D12EnableExperimentalFeatures).\n\n");
        
        OutputDebugString(L"Enabling compute based fallback raytracing support.\n");
        ThrowIfFalse(EnableComputeRaytracingFallback(), L"Could not enable compute based fallback raytracing support (D3D12EnableExperimentalFeatures() failed).\n");
    }

    m_forceComputeFallback = false;
    SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
    
    m_deviceResources = std::make_unique<DeviceResources>(
        DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_UNKNOWN,
        FrameCount,
        D3D_FEATURE_LEVEL_11_0,
        DeviceResources::c_AllowTearing
        );
    m_deviceResources->RegisterDeviceNotify(this);

    // Sample shows handling of use cases with tearing support, which is OS dependent and has been supported since Threshold II.
    // Since the Fallback Layer requires Fall Creator's update (RS3), we don't need to handle non-tearing cases.
    if (!m_deviceResources->IsTearingSupported())
    {
        OutputDebugString(L"Sample must be run on an OS with tearing support.\n");
        exit(EXIT_FAILURE);
    }

    m_camera.SetEyeAtUp(Math::Vector3{0, 0.5, 4}, Math::Vector3{0, 0, 0}, Math::Vector3{0, 1, 0});
    m_camera.ReverseZ(false);

    UpdateForSizeChange(width, height);
}

void D3D12RaytracingHelloWorld::OnInit()
{
    GameCore::g_hWnd = Win32Application::GetHwnd();
    GameInput::Initialize();

    m_deviceResources->SetWindow(Win32Application::GetHwnd(), m_width, m_height);

    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();
}

// Create resources that depend on the device.
void D3D12RaytracingHelloWorld::CreateDeviceDependentResources()
{
    CreateRaytracingDevice();
    CreateRootSignatures();
    CreateRaytracingPipelineStateObject();
    CreateDescriptorHeap();
    CreateRaytracingOutputResource();
    CreateConstantBuffers();
    CreateRandomSamplesBuffer();
    BuildGeometry();
    BuildAccelerationStructures();
    BuildShaderTables();
}

void D3D12RaytracingHelloWorld::SerializeAndCreateRaytracingRootSignature(D3D12_ROOT_SIGNATURE_DESC& desc, ComPtr<ID3D12RootSignature>* rootSig)
{
    auto device = m_deviceResources->GetD3DDevice();
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> error;

    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ThrowIfFailed(m_fallbackDevice->D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
        ThrowIfFailed(m_fallbackDevice->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*rootSig))));
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), error ? static_cast<wchar_t*>(error->GetBufferPointer()) : nullptr);
        ThrowIfFailed(device->CreateRootSignature(1, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&(*rootSig))));
    }
}

void D3D12RaytracingHelloWorld::CreateRootSignatures()
{
    // Global Root Signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    {
        CD3DX12_DESCRIPTOR_RANGE UAVDescriptor;
        UAVDescriptor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        CD3DX12_ROOT_PARAMETER rootParameters[GlobalRootSignatureParams::Count];
        rootParameters[GlobalRootSignatureParams::OutputViewSlot].InitAsDescriptorTable(1, &UAVDescriptor);
        rootParameters[GlobalRootSignatureParams::AccelerationStructureSlot].InitAsShaderResourceView(0);
        rootParameters[GlobalRootSignatureParams::PerFrameCBSlot].InitAsConstantBufferView(1);
        {
            CD3DX12_DESCRIPTOR_RANGE descriptor;
            descriptor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 16);
            rootParameters[GlobalRootSignatureParams::NormalBuffersSlot].InitAsDescriptorTable(1, &descriptor);
        }
        {
            CD3DX12_DESCRIPTOR_RANGE descriptor;
            descriptor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 16, 32);
            rootParameters[GlobalRootSignatureParams::IndexBuffersSlot].InitAsDescriptorTable(1, &descriptor);
        }
        {
            CD3DX12_DESCRIPTOR_RANGE descriptor;
            descriptor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
            rootParameters[GlobalRootSignatureParams::RndSamplesBufferSlot].InitAsDescriptorTable(1, &descriptor);
        }
        CD3DX12_ROOT_SIGNATURE_DESC globalRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        SerializeAndCreateRaytracingRootSignature(globalRootSignatureDesc, &m_raytracingGlobalRootSignature);
    }

    // Local Root Signature
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    {
        CD3DX12_DESCRIPTOR_RANGE UAVDescriptor;
        CD3DX12_ROOT_PARAMETER rootParameters[LocalRootSignatureParams::Count];
        rootParameters[LocalRootSignatureParams::ViewportConstantSlot].InitAsConstants(SizeOfInUint32(sizeof(PerMaterialCB)), 0, 0);
        CD3DX12_ROOT_SIGNATURE_DESC localRootSignatureDesc(ARRAYSIZE(rootParameters), rootParameters);
        localRootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
        SerializeAndCreateRaytracingRootSignature(localRootSignatureDesc, &m_raytracingLocalRootSignature);
    }
}

void D3D12RaytracingHelloWorld::CreateRaytracingDevice()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();

    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        CreateRaytracingFallbackDeviceFlags createDeviceFlags = m_forceComputeFallback ? 
                                                    CreateRaytracingFallbackDeviceFlags::ForceComputeFallback : 
                                                    CreateRaytracingFallbackDeviceFlags::None;
        ThrowIfFailed(D3D12CreateRaytracingFallbackDevice(device, createDeviceFlags, 0, IID_PPV_ARGS(&m_fallbackDevice)));
        m_fallbackDevice->QueryRaytracingCommandList(commandList, IID_PPV_ARGS(&m_fallbackCommandList));
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(device->QueryInterface(__uuidof(ID3D12DeviceRaytracingPrototype), &m_dxrDevice), L"Couldn't get DirectX Raytracing interface for the device.\n");
        ThrowIfFailed(commandList->QueryInterface(__uuidof(ID3D12CommandListRaytracingPrototype), &m_dxrCommandList), L"Couldn't get DirectX Raytracing interface for the command list.\n");
    }
}

// Create a raytracing pipeline state object (RTPSO).
// An RTPSO represents a full set of shaders reachable by a DispatchRays() call,
// with all configuration options resolved, such as local signatures and other state.
void D3D12RaytracingHelloWorld::CreateRaytracingPipelineStateObject()
{
    // Create 7 subobjects that combine into a RTPSO:
    // Subobjects need to be associated with DXIL exports (i.e. shaders) either by way of default or explicit associations.
    // Default association applies to every exported shader entrypoint that doesn't have any of the same type of subobject associated with it.
    // This simple sample utilizes default shader association except for local root signature subobject
    // which has an explicit association specified purely for demonstration purposes.
    // 1 - DXIL library
    // 1 - Triangle hit group
    // 1 - Shader config
    // 2 - Local root signature and association
    // 1 - Global root signature
    // 1 - Pipeline config
    CD3D12_STATE_OBJECT_DESC raytracingPipeline{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE };


    // DXIL library
    // This contains the shaders and their entrypoints for the state object.
    // Since shaders are not considered a subobject, they need to be passed in via DXIL library subobjects.
    auto lib = raytracingPipeline.CreateSubobject<CD3D12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE libdxil = CD3DX12_SHADER_BYTECODE((void *)g_pRaytracing, ARRAYSIZE(g_pRaytracing));
    lib->SetDXILLibrary(&libdxil);
    // Define which shader exports to surface from the library.
    // If no shader exports are defined for a DXIL library subobject, all shaders will be surfaced.
    // In this sample, this could be omitted for convenience since the sample uses all shaders in the library. 
    {
        lib->DefineExport(c_raygenShaderName);
        lib->DefineExport(c_closestHitShaderName);
        lib->DefineExport(c_missShaderName);
    }
    
    // Triangle hit group
    // A hit group specifies closest hit, any hit and intersection shaders to be executed when a ray intersects the geometry's triangle/AABB.
    // In this sample, we only use triangle geometry with a closest hit shader, so others are not set.
    auto hitGroup = raytracingPipeline.CreateSubobject<CD3D12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetClosestHitShaderImport(c_closestHitShaderName);
    hitGroup->SetHitGroupExport(c_hitGroupName);
    
    // Shader config
    // Defines the maximum sizes in bytes for the ray payload and attribute structure.
    auto shaderConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize = 4 * sizeof(float) + 4 * sizeof(uint32_t);   // float4 color
    UINT attributeSize = 2 * sizeof(float); // float2 barycentrics
    shaderConfig->Config(payloadSize, attributeSize);

    // Local root signature and shader association
    // This is a root signature that enables a shader to have unique arguments that come from shader tables.
    auto localRootSignature = raytracingPipeline.CreateSubobject<CD3D12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
    localRootSignature->SetRootSignature(m_raytracingLocalRootSignature.Get());
    // Define explicit shader association for the local root signature. 
    // In this sample, this could be ommited for convenience since it matches the default association.
    {
        auto rootSignatureAssociation = raytracingPipeline.CreateSubobject<CD3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
        rootSignatureAssociation->SetSubobjectToAssociate(*localRootSignature);
        rootSignatureAssociation->AddExport(c_raygenShaderName);
        rootSignatureAssociation->AddExport(c_missShaderName);
        rootSignatureAssociation->AddExport(c_hitGroupName);
    }

    // Global root signature
    // This is a root signature that is shared across all raytracing shaders invoked during a DispatchRays() call.
    auto globalRootSignature = raytracingPipeline.CreateSubobject<CD3D12_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSignature->SetRootSignature(m_raytracingGlobalRootSignature.Get());

    // Pipeline config
    // Defines the maximum TraceRay() recursion depth.
    auto pipelineConfig = raytracingPipeline.CreateSubobject<CD3D12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    // Setting max recursion depth at 1 ~ primary rays only. 
    // Drivers may apply optimization strategies for low recursion depths, 
    // so it is recommended to set max recursion depth as low as needed. 
    pipelineConfig->Config(2);  

#if _DEBUG
    PrintStateObjectDesc(raytracingPipeline);
#endif

    // Create the state object.
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ThrowIfFailed(m_fallbackDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_fallbackStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }
    else // DirectX Raytracing
    {
        ThrowIfFailed(m_dxrDevice->CreateStateObject(raytracingPipeline, IID_PPV_ARGS(&m_dxrStateObject)), L"Couldn't create DirectX Raytracing state object.\n");
    }
}

// Create 2D output texture for raytracing.
void D3D12RaytracingHelloWorld::CreateRaytracingOutputResource()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto backbufferFormat = m_deviceResources->GetBackBufferFormat();

    // Create the output resource. The dimensions and format should match the swap-chain.
    auto uavDesc = CD3DX12_RESOURCE_DESC::Tex2D(backbufferFormat, m_width, m_height, 1, 1, 1, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

    auto defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(
        &defaultHeapProperties, D3D12_HEAP_FLAG_NONE, &uavDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_raytracingOutput)));
    NAME_D3D12_OBJECT(m_raytracingOutput);

    D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptorHandle;
    m_raytracingOutputResourceUAVDescriptorHeapIndex = AllocateDescriptor(&uavDescriptorHandle, m_raytracingOutputResourceUAVDescriptorHeapIndex);
    D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};
    UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    device->CreateUnorderedAccessView(m_raytracingOutput.Get(), nullptr, &UAVDesc, uavDescriptorHandle);
    m_raytracingOutputResourceUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), m_raytracingOutputResourceUAVDescriptorHeapIndex, m_descriptorSize);
}

void D3D12RaytracingHelloWorld::CreateConstantBuffers()
{
    auto device = m_deviceResources->GetD3DDevice();
    
    // Create the constant buffer memory and map the CPU and GPU addresses
    const D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    for (int32_t i = 0; i < FrameCount; ++i)
    {
        // Allocate one constant buffer per frame, since it gets updated every frame.
        const D3D12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(PerFrameCB));

        PerFrameCB cb;
        std::memset(&cb, 0x00, sizeof(cb));
        AllocateUploadBuffer(device, &cb, constantBufferDesc.Width, &m_perFrameCB[i]);
    }
}

void D3D12RaytracingHelloWorld::CreateRandomSamplesBuffer()
{
    auto device = m_deviceResources->GetD3DDevice();

    // Create random samples and counter buffer
    AllocateUAVBuffer(device,
        m_numOfRndSamples * sizeof(RndSamples),
        &m_rndSamples,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        L"RandomSamplesBuffer");
    AllocateUAVBuffer(device,
        sizeof(int32_t),
        &m_rndSamplesCounter,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
        L"RandomSamplesCounterBuffer");

    AllocateUploadBuffer(device,
        nullptr,
        m_numOfRndSamples * sizeof(RndSamples),
        &m_rndSamplesUploadCopy,
        L"RandomSamplesBufferUploadCopy");

    // Create copys on upload heap
    AllocateUploadBuffer(device,
        nullptr,
        sizeof(int32_t),
        &m_rndSamplesCounterUploadCopy,
        L"RandomSamplesCounterBufferUploadCopy");

    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        uavDesc.Buffer.NumElements = m_numOfRndSamples;
        uavDesc.Buffer.StructureByteStride = sizeof(RndSamples);
        uavDesc.Buffer.CounterOffsetInBytes = 0;

        D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
        uint32_t heapIdx = AllocateDescriptor(&descriptor);
        device->CreateUnorderedAccessView(m_rndSamples.Get(), m_rndSamplesCounter.Get(), &uavDesc, descriptor);
        m_rndSamplesUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), heapIdx, m_descriptorSize);
    }

    {
        D3D12_CPU_DESCRIPTOR_HANDLE descriptor;
        uint32_t heapIdx = AllocateDescriptor(&descriptor);
        m_rndSamplesCounterUAVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), heapIdx, m_descriptorSize);
    }
}

void D3D12RaytracingHelloWorld::CreateDescriptorHeap()
{
    auto device = m_deviceResources->GetD3DDevice();

    D3D12_DESCRIPTOR_HEAP_DESC descriptorHeapDesc = {};
    // Allocate a heap for 3 descriptors:
    // 2 - bottom and top level acceleration structure fallback wrapped pointers
    // 1 - raytracing output texture SRV
    // 16 - normal buffers SRV
    // 16 - index buffers SRV
    // 2 - Random samples and counter buffer UAV
    descriptorHeapDesc.NumDescriptors = 3 + 16 + 16 + 2;
    descriptorHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descriptorHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descriptorHeapDesc.NodeMask = 0;
    device->CreateDescriptorHeap(&descriptorHeapDesc, IID_PPV_ARGS(&m_descriptorHeap));
    NAME_D3D12_OBJECT(m_descriptorHeap);

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

// Build geometry used in the sample.
void D3D12RaytracingHelloWorld::BuildGeometry()
{
#if 0
    struct Vertex
    {
        XMFLOAT3 position;
        XMFLOAT3 normal;
    };

    auto device = m_deviceResources->GetD3DDevice();

    // Cube indices.
    uint32_t indices[] =
    {
        3,1,0,
        2,1,3,

        6,4,5,
        7,4,6,

        11,9,8,
        10,9,11,

        14,12,13,
        15,12,14,

        19,17,16,
        18,17,19,

        22,20,21,
        23,20,22
    };

    // Cube vertices positions and corresponding triangle normals.
    Vertex vertices[] =
    {
        { XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },

        { XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, -1.0f, 0.0f) },

        { XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },

        { XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(1.0f, 0.0f, 0.0f) },

        { XMFLOAT3(-1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(1.0f, -1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(-1.0f, 1.0f, -1.0f), XMFLOAT3(0.0f, 0.0f, -1.0f) },

        { XMFLOAT3(-1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(1.0f, -1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-1.0f, 1.0f, 1.0f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
    };

    Primitive prim;
    prim.m_name = "cube";

    AllocateUploadBuffer(device,
        vertices,
        sizeof(vertices),
        &prim.m_positionBuffer);
    //AllocateUploadBuffer(device,
    //    shape.mesh.normals.data(),
    //    shape.mesh.normals.size() * sizeof(decltype(shape.mesh.normals)::value_type),
    //    &prim.m_normalBuffer);
    AllocateUploadBuffer(device,
        indices,
        sizeof(indices),
        &prim.m_indexBuffer);

    prim.m_geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    prim.m_geometryDesc.Triangles.IndexBuffer = prim.m_indexBuffer->GetGPUVirtualAddress();
    prim.m_geometryDesc.Triangles.IndexCount = _ARRAYSIZE(indices);
    prim.m_geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    prim.m_geometryDesc.Triangles.Transform = 0;
    prim.m_geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    prim.m_geometryDesc.Triangles.VertexCount = _ARRAYSIZE(vertices);
    prim.m_geometryDesc.Triangles.VertexBuffer.StartAddress = prim.m_positionBuffer->GetGPUVirtualAddress();
    prim.m_geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);

    m_primitives.push_back(prim);
#else
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string err;
    if (!tinyobj::LoadObj(shapes, materials, err, "CornellBox-Glossy.obj", nullptr))
    {
        assert(false);
    }

    auto device = m_deviceResources->GetD3DDevice();

    for (int i = 0; i < shapes.size(); ++i)
    {
        tinyobj::shape_t& shape = shapes[i];

		Primitive prim;
		prim.m_name = shape.name;

        AllocateUploadBuffer(device,
            shape.mesh.positions.data(),
            shape.mesh.positions.size() * sizeof(decltype(shape.mesh.positions)::value_type),
            &prim.m_positionBuffer);
        AllocateUploadBuffer(device,
            shape.mesh.normals.data(),
            shape.mesh.normals.size() * sizeof(decltype(shape.mesh.normals)::value_type),
            &prim.m_normalBuffer);
        AllocateUploadBuffer(device,
            shape.mesh.indices.data(),
            shape.mesh.indices.size() * sizeof(decltype(shape.mesh.indices)::value_type),
            &prim.m_indexBuffer);

        prim.m_geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        prim.m_geometryDesc.Triangles.IndexBuffer = prim.m_indexBuffer->GetGPUVirtualAddress();
        prim.m_geometryDesc.Triangles.IndexCount = UINT(shape.mesh.indices.size());
        prim.m_geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        prim.m_geometryDesc.Triangles.Transform = 0;
        prim.m_geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        prim.m_geometryDesc.Triangles.VertexCount = UINT(shape.mesh.positions.size()) / 3;
        prim.m_geometryDesc.Triangles.VertexBuffer.StartAddress = prim.m_positionBuffer->GetGPUVirtualAddress();
        prim.m_geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;

        int32_t materialId = shape.mesh.material_ids[0];
        const tinyobj::material_t& material = materials[materialId];
        prim.m_material.diffuse = XMVECTOR{material.diffuse[0], material.diffuse[1], material.diffuse[2], 1};
        prim.m_material.normalBufferIdx = i;
        prim.m_material.indexBufferIdx = i;

		m_primitives.push_back(prim);
    }

    for (int i = 0; i < shapes.size(); ++i)
    {
        tinyobj::shape_t& shape = shapes[i];
        const Primitive& prim = m_primitives[i];

        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle;
        UINT descriptorHeapIndex = AllocateDescriptor(&cpuDescriptorHandle, 0xFFFFFFFF);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = UINT(shape.mesh.normals.size() / 3);
        srvDesc.Buffer.StructureByteStride = sizeof(float) * 3;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        device->CreateShaderResourceView(prim.m_normalBuffer.Get(), &srvDesc, cpuDescriptorHandle);

        if (i == 0)
        {
            m_normalBuffersSRVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorHeapIndex, m_descriptorSize);
        }
    }

    for (int i = int(shapes.size()); i < 16; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle;
        UINT descriptorHeapIndex = AllocateDescriptor(&cpuDescriptorHandle, 0xFFFFFFFF);
    }

    for (int i = 0; i < shapes.size(); ++i)
    {
        tinyobj::shape_t& shape = shapes[i];
        const Primitive& prim = m_primitives[i];

        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle;
        UINT descriptorHeapIndex = AllocateDescriptor(&cpuDescriptorHandle, 0xFFFFFFFF);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = UINT(shape.mesh.indices.size() / 3);
        srvDesc.Buffer.StructureByteStride = sizeof(uint32_t) * 3;
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        device->CreateShaderResourceView(prim.m_indexBuffer.Get(), &srvDesc, cpuDescriptorHandle);

        if (i == 0)
        {
            m_indexBuffersSRVGpuDescriptor = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_descriptorHeap->GetGPUDescriptorHandleForHeapStart(), descriptorHeapIndex, m_descriptorSize);
        }
    }

    for (int i = int(shapes.size()); i < 16; ++i)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuDescriptorHandle;
        UINT descriptorHeapIndex = AllocateDescriptor(&cpuDescriptorHandle, 0xFFFFFFFF);
    }
#endif
}

// Build acceleration structures needed for raytracing.
void D3D12RaytracingHelloWorld::BuildAccelerationStructures()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();
    auto commandQueue = m_deviceResources->GetCommandQueue();
    auto commandAllocator = m_deviceResources->GetCommandAllocator();

    // Reset the command list for the acceleration structure construction.
    commandList->Reset(commandAllocator, nullptr);

    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> allGeometryDescs;
    for (auto p : m_primitives)
    {
        allGeometryDescs.push_back(p.m_geometryDesc);
    }

    // Get required sizes for an acceleration structure.
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS buildFlags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topLevelPrebuildInfo = {};
    {
        D3D12_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_DESC prebuildInfoDesc = {};
        prebuildInfoDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        prebuildInfoDesc.Flags = buildFlags;
        prebuildInfoDesc.NumDescs = 1;
        prebuildInfoDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        prebuildInfoDesc.pGeometryDescs = nullptr;
        if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
        {
            m_fallbackDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfoDesc, &topLevelPrebuildInfo);
        }
        else // DirectX Raytracing
        {
            m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfoDesc, &topLevelPrebuildInfo);
        }
        ThrowIfFalse(topLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);
    }

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottomLevelPrebuildInfo = {};
    {
        D3D12_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_DESC prebuildInfoDesc = {};
        prebuildInfoDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        prebuildInfoDesc.Flags = buildFlags;
        prebuildInfoDesc.NumDescs = UINT(allGeometryDescs.size());
        prebuildInfoDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        prebuildInfoDesc.pGeometryDescs = allGeometryDescs.data();
        if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
        {
            m_fallbackDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfoDesc, &bottomLevelPrebuildInfo);
        }
        else // DirectX Raytracing
        {
            m_dxrDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfoDesc, &bottomLevelPrebuildInfo);
        }
        ThrowIfFalse(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes > 0);
    }

    ComPtr<ID3D12Resource> scratchResource;
    AllocateUAVBuffer(device, max(topLevelPrebuildInfo.ScratchDataSizeInBytes, bottomLevelPrebuildInfo.ScratchDataSizeInBytes), &scratchResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"ScratchResource");

    // Create resources for acceleration structures.
    {
        D3D12_RESOURCE_STATES initialResourceState;
        if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
        {
            initialResourceState = m_fallbackDevice->GetAccelerationStructureResourceState();
        }
        else // DirectX Raytracing
        {
            initialResourceState = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        }

        AllocateUAVBuffer(device, bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes, &m_bottomLevelAccelerationStructure, initialResourceState, L"BottomLevelAccelerationStructure");
        AllocateUAVBuffer(device, topLevelPrebuildInfo.ResultDataMaxSizeInBytes, &m_topLevelAccelerationStructure, initialResourceState, L"TopLevelAccelerationStructure");
    }

    // Create an instance desc for the bottom level acceleration structure.
    ComPtr<ID3D12Resource> instanceDescs;
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        D3D12_RAYTRACING_FALLBACK_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0] = instanceDesc.Transform[5] = instanceDesc.Transform[10] = 1;
        instanceDesc.InstanceMask = 1;
        UINT numBufferElements = static_cast<UINT>(bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes) / sizeof(UINT32);
        instanceDesc.AccelerationStructure = CreateFallbackWrappedPointer(m_bottomLevelAccelerationStructure.Get(), numBufferElements); 
        AllocateUploadBuffer(device, &instanceDesc, sizeof(instanceDesc), &instanceDescs, L"InstanceDescs");
    }
    else // DirectX Raytracing
    {
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        instanceDesc.Transform[0] = instanceDesc.Transform[5] = instanceDesc.Transform[10] = 1;
        instanceDesc.InstanceMask = 1;
        instanceDesc.AccelerationStructure = m_bottomLevelAccelerationStructure->GetGPUVirtualAddress();
        AllocateUploadBuffer(device, &instanceDesc, sizeof(instanceDesc), &instanceDescs, L"InstanceDescs");
    }

    // Create a wrapped pointer to the acceleration structure.
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        UINT numBufferElements = static_cast<UINT>(topLevelPrebuildInfo.ResultDataMaxSizeInBytes) / sizeof(UINT32);
        m_fallbackTopLevelAccelerationStructurePointer = CreateFallbackWrappedPointer(m_topLevelAccelerationStructure.Get(), numBufferElements); 
    }

    // Bottom Level Acceleration Structure desc
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bottomLevelBuildDesc = {};
    {
        bottomLevelBuildDesc.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bottomLevelBuildDesc.Flags = buildFlags;
        bottomLevelBuildDesc.ScratchAccelerationStructureData = { scratchResource->GetGPUVirtualAddress(), scratchResource->GetDesc().Width };
        bottomLevelBuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bottomLevelBuildDesc.DestAccelerationStructureData = { m_bottomLevelAccelerationStructure->GetGPUVirtualAddress(), bottomLevelPrebuildInfo.ResultDataMaxSizeInBytes };
        bottomLevelBuildDesc.NumDescs = UINT(allGeometryDescs.size());
        bottomLevelBuildDesc.pGeometryDescs = allGeometryDescs.data();
    }

    // Top Level Acceleration Structure desc
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC topLevelBuildDesc = bottomLevelBuildDesc;
    {
        topLevelBuildDesc.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        topLevelBuildDesc.DestAccelerationStructureData = { m_topLevelAccelerationStructure->GetGPUVirtualAddress(), topLevelPrebuildInfo.ResultDataMaxSizeInBytes };
        topLevelBuildDesc.NumDescs = 1;
        topLevelBuildDesc.pGeometryDescs = nullptr;
        topLevelBuildDesc.InstanceDescs = instanceDescs->GetGPUVirtualAddress();
        topLevelBuildDesc.ScratchAccelerationStructureData = { scratchResource->GetGPUVirtualAddress(), scratchResource->GetDesc().Width };
    }

    auto BuildAccelerationStructure = [&](auto* raytracingCommandList)
    {
        raytracingCommandList->BuildRaytracingAccelerationStructure(&bottomLevelBuildDesc);
        commandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::UAV(m_bottomLevelAccelerationStructure.Get()));
        raytracingCommandList->BuildRaytracingAccelerationStructure(&topLevelBuildDesc);
    };

    // Build acceleration structure.
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        // Set the descriptor heaps to be used during acceleration structure build for the Fallback Layer.
        ID3D12DescriptorHeap *pDescriptorHeaps[] = { m_descriptorHeap.Get() };
        m_fallbackCommandList->SetDescriptorHeaps(ARRAYSIZE(pDescriptorHeaps), pDescriptorHeaps);
        BuildAccelerationStructure(m_fallbackCommandList.Get());
    }
    else // DirectX Raytracing
    {
        BuildAccelerationStructure(m_dxrCommandList.Get());
    }

    // Kick off acceleration structure construction.
    m_deviceResources->ExecuteCommandList();

    // Wait for GPU to finish as the locally created temporary GPU resources will get released once we go out of scope.
    m_deviceResources->WaitForGpu();
}

// Build shader tables.
// This encapsulates all shader records - shaders and the arguments for their local root signatures.
void D3D12RaytracingHelloWorld::BuildShaderTables()
{
    auto device = m_deviceResources->GetD3DDevice();

    void* rayGenShaderIdentifier;
    void* missShaderIdentifier;
    void* hitGroupShaderIdentifier;

    auto GetShaderIdentifiers = [&](auto* stateObjectProperties)
    {
        rayGenShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_raygenShaderName);
        missShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_missShaderName);
        hitGroupShaderIdentifier = stateObjectProperties->GetShaderIdentifier(c_hitGroupName);
    };

    // Get shader identifiers.
    UINT shaderIdentifierSize;
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        GetShaderIdentifiers(m_fallbackStateObject.Get());
        shaderIdentifierSize = m_fallbackDevice->GetShaderIdentifierSize();
    }
    else // DirectX Raytracing
    {
        ComPtr<ID3D12StateObjectPropertiesPrototype> stateObjectProperties;
        ThrowIfFailed(m_dxrStateObject.As(&stateObjectProperties));
        GetShaderIdentifiers(stateObjectProperties.Get());
        shaderIdentifierSize = m_dxrDevice->GetShaderIdentifierSize();
    }

    // Initialize shader records.
    assert(LocalRootSignatureParams::ViewportConstantSlot == 0  && LocalRootSignatureParams::Count == 1);
    struct RootArguments {
        PerMaterialCB cb;
    } rootArguments;
    UINT rootArgumentsSize = sizeof(rootArguments);

    // Shader record = {{ Shader ID }, { RootArguments }}
    m_shaderRecordSize = shaderIdentifierSize + rootArgumentsSize;

    ShaderRecord rayGenShaderRecord(rayGenShaderIdentifier, shaderIdentifierSize, &rootArguments, rootArgumentsSize);
    rayGenShaderRecord.AllocateAsUploadBuffer(device, &m_rayGenShaderTable, L"RayGenShaderTable");

    ShaderRecord missShaderRecord(missShaderIdentifier, shaderIdentifierSize, &rootArguments, rootArgumentsSize);
    missShaderRecord.AllocateAsUploadBuffer(device, &m_missShaderTable, L"MissShaderTable");

    std::vector<ShaderRecord::PointerWithSize> hitGroupShadersTbl;
    std::vector<ShaderRecord::PointerWithSize> rootArgumentsTbl;

    for (int32_t i = 0; i < m_primitives.size(); ++i)
    {
        hitGroupShadersTbl.push_back(ShaderRecord::PointerWithSize{hitGroupShaderIdentifier, shaderIdentifierSize});
        rootArgumentsTbl.push_back(ShaderRecord::PointerWithSize{&m_primitives[i].m_material, rootArgumentsSize});
    }
    ShaderRecord hitGroupShaderRecord(hitGroupShadersTbl, rootArgumentsTbl);
    hitGroupShaderRecord.AllocateAsUploadBuffer(device, &m_hitGroupShaderTable, L"HitGroupShaderTable");
}

void D3D12RaytracingHelloWorld::SelectRaytracingAPI(RaytracingAPI type)
{
    if (type == RaytracingAPI::FallbackLayer)
    {
        m_raytracingAPI = type;
    }
    else // DirectX Raytracing
    {
        if (m_isDxrSupported)
        {
            m_raytracingAPI = type;
        }
        else
        {
            OutputDebugString(L"Invalid selection - DXR is not available.\n");
        }
    }
}

void D3D12RaytracingHelloWorld::OnKeyDown(UINT8 key)
{
    // Store previous values.
    RaytracingAPI previousRaytracingAPI = m_raytracingAPI;
    bool previousForceComputeFallback = m_forceComputeFallback;

    switch (key)
    {
    case '1': // Fallback Layer
        m_forceComputeFallback = false;
        SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
        break;
    case '2': // Fallback Layer + force compute path
        m_forceComputeFallback = true;
        SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
        break;
    case '3': // DirectX Raytracing
        SelectRaytracingAPI(RaytracingAPI::DirectXRaytracing);
        break;
    default:
        break;
    }
    
    if (m_raytracingAPI != previousRaytracingAPI ||
        m_forceComputeFallback != previousForceComputeFallback)
    {
        // Raytracing API selection changed, recreate everything.
        RecreateD3D();
    }
}

// Update frame-based values.
void D3D12RaytracingHelloWorld::OnUpdate()
{
    m_timer.Tick();
    CalculateFrameStats();

    float delta = static_cast<float>(m_timer.GetElapsedSeconds());
    GameInput::Update(delta);
    m_cameraController.Update(delta);

    m_camera.Update();
    m_perFrameCBContent.viewToWorld = Math::Invert(m_camera.GetViewMatrix());
    m_perFrameCBContent.projectionToWorld = Math::Invert(m_camera.GetViewProjMatrix());

    if (abs(GameInput::GetAnalogInput(GameInput::kAnalogMouseX)) > 0.0 || abs(GameInput::GetAnalogInput(GameInput::kAnalogMouseY)) > 0.0)
    {
        m_iter = 0;
    }

    if (0)
    {
        XMVECTOR m_eye = { 0.0f, 2.0f, -5.0f, 1.0f };
        XMVECTOR m_at = { 0.0f, 0.0f, 0.0f, 1.0f };
        XMVECTOR right = { 1.0f, 0.0f, 0.0f, 0.0f };

        XMVECTOR direction = XMVector4Normalize(m_at - m_eye);
        XMVECTOR m_up = XMVector3Normalize(XMVector3Cross(direction, right));

        // Rotate camera around Y axis.
        XMMATRIX rotate = XMMatrixRotationY(XMConvertToRadians(45.0f));
        m_eye = XMVector3Transform(m_eye, rotate);
        m_up = XMVector3Transform(m_up, rotate);

        float fovAngleY = 45.0f;
        XMMATRIX view = XMMatrixLookAtLH(m_eye, m_at, m_up);
        XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(fovAngleY), m_aspectRatio, 1.0f, 125.0f);
        XMMATRIX viewProj = view * proj;

        m_perFrameCBContent.cameraPosition = m_eye;
        m_perFrameCBContent.viewToWorld = XMMatrixInverse(nullptr, view);
        m_perFrameCBContent.projectionToWorld = XMMatrixInverse(nullptr, viewProj);
    }
}


// Parse supplied command line args.
void D3D12RaytracingHelloWorld::ParseCommandLineArgs(WCHAR* argv[], int argc)
{
    if (argc > 1)
    {
        if (_wcsnicmp(argv[1], L"-FL", wcslen(argv[1])) == 0 )
        {
            m_forceComputeFallback = true;
            SelectRaytracingAPI(RaytracingAPI::FallbackLayer);
        }
        else if (_wcsnicmp(argv[1], L"-DXR", wcslen(argv[1])) == 0)
        {
            SelectRaytracingAPI(RaytracingAPI::DirectXRaytracing);
        }
    }
}

void D3D12RaytracingHelloWorld::DoRaytracing()
{
    auto commandList = m_deviceResources->GetCommandList();
    

    auto DispatchRays = [&](auto* commandList, auto* stateObject, auto* dispatchDesc)
    {
        dispatchDesc->HitGroupTable.StartAddress = m_hitGroupShaderTable->GetGPUVirtualAddress();
        dispatchDesc->HitGroupTable.SizeInBytes = m_hitGroupShaderTable->GetDesc().Width;
        dispatchDesc->HitGroupTable.StrideInBytes = m_shaderRecordSize;
        dispatchDesc->MissShaderTable.StartAddress = m_missShaderTable->GetGPUVirtualAddress();
        dispatchDesc->MissShaderTable.SizeInBytes = m_missShaderTable->GetDesc().Width;
        dispatchDesc->MissShaderTable.StrideInBytes = dispatchDesc->MissShaderTable.SizeInBytes;
        dispatchDesc->RayGenerationShaderRecord.StartAddress = m_rayGenShaderTable->GetGPUVirtualAddress();
        dispatchDesc->RayGenerationShaderRecord.SizeInBytes = m_rayGenShaderTable->GetDesc().Width;
        dispatchDesc->Width = m_width;
        dispatchDesc->Height = m_height;
        commandList->DispatchRays(stateObject, dispatchDesc);
    };

    commandList->SetComputeRootSignature(m_raytracingGlobalRootSignature.Get());

    // Bind the heaps, acceleration structure and dispatch rays.    
    if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
    {
        ID3D12Resource* pCB = m_perFrameCB[m_cbIdx].Get();
        m_cbIdx = (m_cbIdx + 1) % FrameCount;

        ++m_iter;
        m_perFrameCBContent.weight = XMVECTOR{(m_iter-1)/m_iter, 1/m_iter, 0, 0};

        void* pMappedData = nullptr;
        pCB->Map(0, nullptr, &pMappedData);
        memcpy(pMappedData, &m_perFrameCBContent, sizeof(m_perFrameCBContent));
        pCB->Unmap(0, nullptr);

        D3D12_FALLBACK_DISPATCH_RAYS_DESC dispatchDesc = {};
        m_fallbackCommandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());


        {
            {
                uint32_t initialCounter = 0;
                void* pMappedData;
                m_rndSamplesCounterUploadCopy->Map(0, nullptr, &pMappedData);
                memcpy(pMappedData, &initialCounter, sizeof(initialCounter));
                m_rndSamplesCounterUploadCopy->Unmap(0, nullptr);
            }

            {
#if 0
                if (m_allRndSamples.get() == nullptr)
                {
                    m_allRndSamples = unique_ptr<RndSamples[]>{ new RndSamples[m_numAllRndSamples] };

                    std::random_device rd;
                    std::mt19937 gen(rd());
                    std::uniform_real_distribution<float> dis(0.0f, 1.0f);

                    for (int32_t i = 0; i < m_numAllRndSamples; ++i)
                    {
                        m_allRndSamples[i].p0 = dis(gen);
                        m_allRndSamples[i].p1 = dis(gen);
                    }
                }

                int32_t sampleIdx = (std::rand() * 256) % m_numAllRndSamples;
                int32_t sz0 = min(m_numAllRndSamples - sampleIdx, m_numOfRndSamples);
                int32_t sz1 = max(m_numOfRndSamples - sz0, 0);
                assert(sz0 > 0 && sz1 >= 0 && (sz0 + sz1) == m_numOfRndSamples);

                void* pMappedData;
                m_rndSamplesUploadCopy->Map(0, nullptr, &pMappedData);
                memcpy(pMappedData, (RndSamples*)m_allRndSamples.get() + sampleIdx, sz0 * sizeof(RndSamples));
                if (sz1 > 0)
                {
                    memcpy((RndSamples*)pMappedData + sz0, (RndSamples*)m_allRndSamples.get(), sz1 * sizeof(RndSamples));
                }
                m_rndSamplesUploadCopy->Unmap(0, nullptr);
#else
                unique_ptr<RndSamples[]> rndSamples = unique_ptr<RndSamples[]>{ new RndSamples[m_numOfRndSamples] };

                std::random_device rd;
                std::mt19937 gen(rd());
                std::uniform_real_distribution<float> dis(0.0f, 1.0f);

                for (int32_t i = 0; i < m_numOfRndSamples; ++i)
                {
                    rndSamples[i].p0 = dis(gen);
                    rndSamples[i].p1 = dis(gen);
                }

                void* pMappedData;
                m_rndSamplesUploadCopy->Map(0, nullptr, &pMappedData);
                memcpy(pMappedData, rndSamples.get(), m_numOfRndSamples * sizeof(RndSamples));
                m_rndSamplesUploadCopy->Unmap(0, nullptr);
#endif
            }

            {
                D3D12_RESOURCE_BARRIER preCopyBarriers[2];
                preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_rndSamples.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_rndSamplesCounter.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
                commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);
            }

            commandList->CopyResource(m_rndSamples.Get(), m_rndSamplesUploadCopy.Get());
            commandList->CopyResource(m_rndSamplesCounter.Get(), m_rndSamplesCounterUploadCopy.Get());

            {
                D3D12_RESOURCE_BARRIER afterCopyBarriers[2];
                afterCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_rndSamples.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                afterCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_rndSamplesCounter.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                commandList->ResourceBarrier(ARRAYSIZE(afterCopyBarriers), afterCopyBarriers);
            }
        }



        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, m_raytracingOutputResourceUAVGpuDescriptor);
        m_fallbackCommandList->SetTopLevelAccelerationStructure(GlobalRootSignatureParams::AccelerationStructureSlot, m_fallbackTopLevelAccelerationStructurePointer);
        commandList->SetComputeRootConstantBufferView(GlobalRootSignatureParams::PerFrameCBSlot, pCB->GetGPUVirtualAddress());
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::NormalBuffersSlot, m_normalBuffersSRVGpuDescriptor);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::IndexBuffersSlot, m_indexBuffersSRVGpuDescriptor);
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::RndSamplesBufferSlot, m_rndSamplesUAVGpuDescriptor);
        DispatchRays(m_fallbackCommandList.Get(), m_fallbackStateObject.Get(), &dispatchDesc);
    }
    else // DirectX Raytracing
    {
        D3D12_DISPATCH_RAYS_DESC dispatchDesc = {};
        commandList->SetDescriptorHeaps(1, m_descriptorHeap.GetAddressOf());
        commandList->SetComputeRootDescriptorTable(GlobalRootSignatureParams::OutputViewSlot, m_raytracingOutputResourceUAVGpuDescriptor);
        commandList->SetComputeRootShaderResourceView(GlobalRootSignatureParams::AccelerationStructureSlot, m_topLevelAccelerationStructure->GetGPUVirtualAddress());
        DispatchRays(m_dxrCommandList.Get(), m_dxrStateObject.Get(), &dispatchDesc);
    }
}

// Update the application state with the new resolution.
void D3D12RaytracingHelloWorld::UpdateForSizeChange(UINT width, UINT height)
{
    DXSample::UpdateForSizeChange(width, height);
}

// Copy the raytracing output to the backbuffer.
void D3D12RaytracingHelloWorld::CopyRaytracingOutputToBackbuffer()
{
    auto commandList= m_deviceResources->GetCommandList();
    auto renderTarget = m_deviceResources->GetRenderTarget();

    D3D12_RESOURCE_BARRIER preCopyBarriers[2];
    preCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    preCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    commandList->ResourceBarrier(ARRAYSIZE(preCopyBarriers), preCopyBarriers);

    commandList->CopyResource(renderTarget, m_raytracingOutput.Get());

    D3D12_RESOURCE_BARRIER postCopyBarriers[2];
    postCopyBarriers[0] = CD3DX12_RESOURCE_BARRIER::Transition(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    postCopyBarriers[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_raytracingOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    commandList->ResourceBarrier(ARRAYSIZE(postCopyBarriers), postCopyBarriers);
}

// Create resources that are dependent on the size of the main window.
void D3D12RaytracingHelloWorld::CreateWindowSizeDependentResources()
{
    CreateRaytracingOutputResource(); 

    // For simplicity, we will rebuild the shader tables.
    BuildShaderTables();
}

// Release resources that are dependent on the size of the main window.
void D3D12RaytracingHelloWorld::ReleaseWindowSizeDependentResources()
{
    m_rayGenShaderTable.Reset();
    m_missShaderTable.Reset();
    m_hitGroupShaderTable.Reset();
    m_raytracingOutput.Reset();
}

// Release all resources that depend on the device.
void D3D12RaytracingHelloWorld::ReleaseDeviceDependentResources()
{
    m_fallbackDevice.Reset();
    m_fallbackCommandList.Reset();
    m_fallbackStateObject.Reset();
    m_raytracingGlobalRootSignature.Reset();
    m_raytracingLocalRootSignature.Reset();
    
    m_dxrDevice.Reset();
    m_dxrCommandList.Reset();
    m_dxrStateObject.Reset();

    m_descriptorHeap.Reset();
    m_descriptorsAllocated = 0;
    m_raytracingOutputResourceUAVDescriptorHeapIndex = UINT_MAX;

    m_accelerationStructure.Reset();
    m_bottomLevelAccelerationStructure.Reset();
    m_topLevelAccelerationStructure.Reset();
}

void D3D12RaytracingHelloWorld::RecreateD3D()
{
    // Give GPU a chance to finish its execution in progress.
    try
    {
        m_deviceResources->WaitForGpu();
    }
    catch (HrException&)
    {
        // Do nothing, currently attached adapter is unresponsive.
    }
    m_deviceResources->HandleDeviceLost();
}

// Render the scene.
void D3D12RaytracingHelloWorld::OnRender()
{
    if (!m_deviceResources->IsWindowVisible())
    {
        return;
    }

    m_deviceResources->Prepare();

    DoRaytracing();
    CopyRaytracingOutputToBackbuffer();

    m_deviceResources->Present(D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12RaytracingHelloWorld::OnDestroy()
{
    OnDeviceLost();
}

// Release all device dependent resouces when a device is lost.
void D3D12RaytracingHelloWorld::OnDeviceLost()
{
    ReleaseWindowSizeDependentResources();
    ReleaseDeviceDependentResources();
}

// Create all device dependent resources when a device is restored.
void D3D12RaytracingHelloWorld::OnDeviceRestored()
{
    CreateDeviceDependentResources();
    CreateWindowSizeDependentResources();
}

// Compute the average frames per second and million rays per second.
void D3D12RaytracingHelloWorld::CalculateFrameStats()
{
    static int frameCnt = 0;
    static double elapsedTime = 0.0f;
    double totalTime = m_timer.GetTotalSeconds();
    frameCnt++;

    // Compute averages over one second period.
    if ((totalTime - elapsedTime) >= 1.0f)
    {
        float diff = static_cast<float>(totalTime - elapsedTime);
        float fps = static_cast<float>(frameCnt) / diff; // Normalize to an exact second.

        frameCnt = 0;
        elapsedTime = totalTime;

        float MRaysPerSecond = (m_width * m_height * fps) / static_cast<float>(1e6);

        wstringstream windowText;

        if (m_raytracingAPI == RaytracingAPI::FallbackLayer)
        {
            if (m_fallbackDevice->UsingRaytracingDriver())
            {
                windowText << L"(FL-DXR)";
            }
            else
            {
                windowText << L"(FL)";
            }
        }
        else
        {
            windowText << L"(DXR)";
        }
        windowText << setprecision(2) << fixed
            << L"    fps: " << fps << L"     ~Million Primary Rays/s: " << MRaysPerSecond;
        SetCustomWindowText(windowText.str().c_str());
    }
}

// Handle OnSizeChanged message event.
void D3D12RaytracingHelloWorld::OnSizeChanged(UINT width, UINT height, bool minimized)
{
    if (!m_deviceResources->WindowSizeChanged(width, height, minimized))
    {
        return;
    }

    UpdateForSizeChange(width, height);

    ReleaseWindowSizeDependentResources();
    CreateWindowSizeDependentResources();
}

// Create a wrapped pointer for the Fallback Layer path.
WRAPPED_GPU_POINTER D3D12RaytracingHelloWorld::CreateFallbackWrappedPointer(ID3D12Resource* resource, UINT bufferNumElements)
{
    auto device = m_deviceResources->GetD3DDevice();

    D3D12_UNORDERED_ACCESS_VIEW_DESC rawBufferUavDesc = {};
    rawBufferUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    rawBufferUavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    rawBufferUavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    rawBufferUavDesc.Buffer.NumElements = bufferNumElements;

    D3D12_CPU_DESCRIPTOR_HANDLE bottomLevelDescriptor;
   
    // Only compute fallback requires a valid descriptor index when creating a wrapped pointer.
    UINT descriptorHeapIndex = 0;
    if (!m_fallbackDevice->UsingRaytracingDriver())
    {
        descriptorHeapIndex = AllocateDescriptor(&bottomLevelDescriptor);
        device->CreateUnorderedAccessView(resource, nullptr, &rawBufferUavDesc, bottomLevelDescriptor);
    }
    return m_fallbackDevice->GetWrappedPointerSimple(descriptorHeapIndex, resource->GetGPUVirtualAddress());
}

// Allocate a descriptor and return its index. 
// If the passed descriptorIndexToUse is valid, it will be used instead of allocating a new one.
UINT D3D12RaytracingHelloWorld::AllocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor, UINT descriptorIndexToUse)
{
    auto descriptorHeapCpuBase = m_descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    if (descriptorIndexToUse >= m_descriptorHeap->GetDesc().NumDescriptors)
    {
        descriptorIndexToUse = m_descriptorsAllocated++;
    }
    *cpuDescriptor = CD3DX12_CPU_DESCRIPTOR_HANDLE(descriptorHeapCpuBase, descriptorIndexToUse, m_descriptorSize);
    return descriptorIndexToUse;
}
