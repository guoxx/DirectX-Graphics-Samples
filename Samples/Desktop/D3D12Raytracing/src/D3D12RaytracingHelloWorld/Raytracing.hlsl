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
RWStructuredBuffer<float2> g_rndSamplesBuffer : register(u1);

ConstantBuffer<PerMaterialCB> materialCB : register(b0);

typedef BuiltInTriangleIntersectionAttributes MyAttributes;
struct HitData
{
    float4 color;
    uint shading;
    uint hit;
    uint2 dummy;
};

float3 HitWorldPosition()
{
    return WorldRayOrigin() + RayTCurrent() * WorldRayDirection();
}

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

float4 ApplySRGBCurve( float4 x )
{
    // Approximately pow(x, 1.0 / 2.2)
    return x < 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
}

float4 RemoveSRGBCurve( float4 x )
{
    // Approximately pow(x, 2.2)
    return x < 0.04045 ? x / 12.92 : pow( (x + 0.055) / 1.055, 2.4 );
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
    HitData payload = { float4(0, 0, 0, 0), 1, 0, uint2(0, 0) };
    TraceRay(Scene, 0, ~0, 0, 1, 0, myRay, payload);

    // Write the raytraced color to the output texture.
    float4 src = RemoveSRGBCurve(RenderTarget[DispatchRaysIndex()]);
    RenderTarget[DispatchRaysIndex()] = ApplySRGBCurve(src * g_perFrameCB.weight.x + payload.color * g_perFrameCB.weight.y);
}

float3 HitAttribute(float3 vertexAttribute[3], BuiltInTriangleIntersectionAttributes attr)
{
    return vertexAttribute[0] +
        attr.barycentrics.x * (vertexAttribute[1] - vertexAttribute[0]) +
        attr.barycentrics.y * (vertexAttribute[2] - vertexAttribute[0]);
}

float2 uniformDistSample()
{
    uint numStructs, stride;
    g_rndSamplesBuffer.GetDimensions(numStructs, stride);
    uint idx = g_rndSamplesBuffer.IncrementCounter();
    idx = idx % numStructs;
    return g_rndSamplesBuffer[idx];
}

void sampleCosineDist(float2 sampleVal, out float3 w, out float pdf)
{
    float theta = acos(1.0 - 2.0 * sampleVal[0]) / 2;
    float phi = sampleVal.y * 2 * MATH_CONST_PI;
    w = float3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
    pdf = cos(theta) / MATH_CONST_PI;
}

float3x3 tangentToWorld(float3 normal)
{
    normal = normalize(normal);

    float3 tangent; 

    float3 c1 = cross(normal, float3(0.0, 0.0, 1.0)); 
    float3 c2 = cross(normal, float3(0.0, 1.0, 0.0)); 

    if( length(c1) > length(c2) )
    {
        tangent = c1;	
    }
    else
    {
        tangent = c2;	
    }

    tangent = normalize(tangent);

    float3 bitangent = cross(tangent, normal);
    return float3x3(tangent, bitangent, normal);
}

[shader("closesthit")]
void MyClosestHitShader(inout HitData payload : SV_RayPayload, in MyAttributes attr : SV_IntersectionAttributes)
{
    if (payload.shading > 0)
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

        float3x3 mTangentToWorld = tangentToWorld(normal);

        float3 w;
        float pdf;
        sampleCosineDist(uniformDistSample(), w, pdf);
        float3 wo = normalize(mul(w, mTangentToWorld));

        // Trace the ray.
        RayDesc shadowRay;
        shadowRay.Origin = HitWorldPosition();
        shadowRay.Direction = wo;
        shadowRay.TMin = 0.00001;
        shadowRay.TMax = 100000;
        HitData shadowPayload = {float4(0, 0, 0, 0), 0, 0, uint2(0, 0)};
        TraceRay(Scene, 0, ~0, 0, 1, 0, shadowRay, shadowPayload);

        //float ao = (shadowPayload.hit > 0 ? 0.0 : 1.0) / pdf * w.z / MATH_CONST_PI;
        float ao = (shadowPayload.hit > 0 ? 0.0 : 1.0);

        payload.color = float4(ao.xxx, 1);
    }
    else
    {
        payload.hit = 1;
    }
}

[shader("miss")]
void MyMissShader(inout HitData payload : SV_RayPayload)
{
    payload.color = float4(0, 0, 0, 1);
    payload.hit = 0;
}

struct ShadowPayload
{
    bool hit;
};

[shader("closesthit")]
 void shadowChs(inout ShadowPayload payload : SV_RayPayload, MyAttributes attribs : SV_IntersectionAttributes)
{
    payload.hit = true;
}

[shader("miss")]
void shadowMiss(inout ShadowPayload payload : SV_RayPayload)
{
    payload.hit = false;
}
