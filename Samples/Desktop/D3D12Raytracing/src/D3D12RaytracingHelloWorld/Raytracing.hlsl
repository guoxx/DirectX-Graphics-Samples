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

#include "RaytracingHlslCompat.h"

RaytracingAccelerationStructure Scene : register(t0, space0);
RWTexture2D<float4> RenderTarget : register(u0);
ConstantBuffer<PerFrameCB> g_perFrameCB: register(b1);
StructuredBuffer<float3> g_normalBuffers[16] : register(t16);
StructuredBuffer<uint3> g_indicesBuffers[16] : register(t32);

ConstantBuffer<PerMaterialCB> materialCB : register(b0);

typedef BuiltInTriangleIntersectionAttributes MyAttributes;
struct HitData
{
    float4 color;
};

inline void GenerateCameraRay(uint2 index, out float3 origin, out float3 direction)
{
    float2 ndc = (index + 0.5) / DispatchRaysDimensions() * 2.0 - 1.0;
    ndc.y = -ndc.y;

    float4 originCS = float4(0, 0, 0, 1);
    float4 targetCS = float4(ndc, 1, 1);
    float4 originWS = mul(originCS, g_perFrameCB.viewToWorld);
    originWS /= originWS.w;
    float4 targetWS = mul(targetCS, g_perFrameCB.projectionToWorld);
    targetWS /= targetWS.w;

    origin = originWS.xyz;
    direction = normalize(targetWS.xyz - origin);
}

[shader("raygeneration")]
void MyRaygenShader()
{
    float3 rayDir;
    float3 origin;
    
    // Generate a ray for a camera pixel corresponding to an index from the dispatched 2D grid.
    GenerateCameraRay(DispatchRaysIndex(), origin, rayDir);

    // Trace the ray.
    RayDesc myRay = { origin, 0.0f, rayDir, 10000.0f };
    HitData payload = { float4(0, 0, 0, 0) };
    //TraceRay(Scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, ~0, 0, 1, 0, myRay, payload);
    // TODO
    TraceRay(Scene, 0, ~0, 0, 1, 0, myRay, payload);

    // Write the raytraced color to the output texture.
    RenderTarget[DispatchRaysIndex()] = payload.color;
}

float3 HitAttribute(float3 vertexAttribute[3], BuiltInTriangleIntersectionAttributes attr)
{
    return vertexAttribute[0] +
        attr.barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        attr.barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

[shader("closesthit")]
void MyClosestHitShader(inout HitData payload : SV_RayPayload, in MyAttributes attr : SV_IntersectionAttributes)
{
    StructuredBuffer<uint3> indexBuffer = g_indicesBuffers[materialCB.indexBufferIdx];
    uint3 indices = indexBuffer[PrimitiveIndex()];

    StructuredBuffer<float3> normalBuffer = g_normalBuffers[materialCB.normalBufferIdx];
    float3 vertexNormals[3] = { 
        normalBuffer[indices.x],
        normalBuffer[indices.y], 
        normalBuffer[indices.z] 
    };

    float3 normal = HitAttribute(vertexNormals, attr);

    float3 barycentrics = float3(1 - attr.barycentrics.x - attr.barycentrics.y, attr.barycentrics.x, attr.barycentrics.y);
    //payload.color = float4(materialCB.diffuse.xyz, 1);
    payload.color = float4(normal, 1);
}

[shader("miss")]
void MyMissShader(inout HitData payload : SV_RayPayload)
{
    payload.color = float4(0, 0, 0, 1);
}