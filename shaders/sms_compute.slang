// Common compute source, specialized by the renderer's concrete scene/hit/texture
// types and three read-only scene arrays. Include after the renderer's imports.
// The scene supplies trace/project for roots and raw occluded for visibility;
// never infer visibility from successful shading-normal reconstruction.
#ifndef SMS_HELPERS_ONLY
[[vk::push_constant]] SmsQueuePush pc;
#endif
#ifndef SMS_STORE_LOCAL_TARGET
#define SMS_STORE_LOCAL_TARGET(slot,work,value)
#endif
#ifndef SMS_DIAG_COMPLETE
#define SMS_DIAG_COMPLETE(reciprocal,conditionalMass,reciprocalWeight)
#endif
#ifndef SMS_CONTRIBUTION_ZERO
#define SMS_CONTRIBUTION_ZERO 0.0.xxx
#endif
#ifndef SMS_FINISH_CONTRIBUTION
#define SMS_FINISH_CONTRIBUTION(base,response,geometric,Le,pdf,prefix,value) \
    value = base * response * geometric * Le / pdf * prefix; \
    return all(isfinite(value)) && all(value >= 0.0.xxx);
#endif

bool prepareLight(inout SmsWorkRecord work) {
    uint count = work.words[13].z;
    if (count == 0u) return false;
    float4 u = smsTrialSample(work.words[7].zw, uint2(0,0), uint2(0xFFFFFFFEu,0));
    uint lo = 0u, hi = count - 1u;
    while (lo < hi) {
        uint middle = lo + (hi - lo) / 2u;
        if (u.z < SMS_EMITTER_CDF[middle]) hi = middle;
        else lo = middle + 1u;
    }
    GpuEmissiveTriangle light = SMS_EMITTERS[lo];
    float previousCdf = lo == 0u ? 0.0 : SMS_EMITTER_CDF[lo - 1u];
    float mass = smsEmitterSelectionMass(SMS_EMITTER_CDF[lo], previousCdf);
    if (!smsProposableEmitter(light, SMS_MATERIALS[light.kind_pad.y], mass)) return false;
    float3 position, normal;
    float area;
    if (light.kind_pad.x == kEmissiveTriangle) {
        float s = sqrt(u.x);
        position = light.v0_area.xyz + light.edge1_emitR.xyz * (1.0 - s) + light.edge2_emitG.xyz * (u.y * s);
        normal = light.normal_emitB.xyz;
        area = light.v0_area.w;
    } else {
        float z = 1.0 - 2.0 * u.x;
        float r = sqrt(max(0.0, 1.0 - z*z)), phi = 2.0*kPi*u.y;
        normal = float3(r*cos(phi), r*sin(phi), z);
        position = light.v0_area.xyz + light.v0_area.w * normal;
        area = 4.0*kPi*light.v0_area.w*light.v0_area.w;
    }
    if (!(area > 0.0)) return false;
    work.words[5] = asuint(float4(position, mass / area));
    work.words[6] = asuint(float4(normal, light.edge1_emitR.w));
    work.words[7].xy = asuint(float2(light.edge2_emitG.w, light.normal_emitB.w));
    work.words[13].z=lo; // freeze identity after consuming emitter count
    return true;
}

bool samplePath(inout SMS_SCENE scene, float3 receiver, float3 light, float4 u,
    uint maxDepth,uint resolution,out SmsManifoldVertex root[2],out uint count) {
    root[0] = root[1] = (SmsManifoldVertex)0;
    count = 0u;
    float z = 1.0 - 2.0*u.x, r = sqrt(max(0.0, 1.0-z*z)), phi = 2.0*kPi*u.y;
    SMS_HIT hit;
    if (!scene.trace(receiver, float3(r*cos(phi), r*sin(phi), z), kInf, hit)) return false;
    GpuMaterial mat = SMS_MATERIALS[hit.vertex.objectKey.y];
    if (smsLegacyMirror(mat)) count = 1u;
    else if (smsSmoothGlass(mat)) count = u.z < 0.5 ? 1u : 2u;
    else return false;
    if (maxDepth < count + 2u) return false;
    SmsManifoldVertex reference[2];
    reference[0] = hit.vertex;
    reference[1] = (SmsManifoldVertex)0;
    reference[0].event = count == 1u ? kSmsReflection : kSmsTransmission;
    if (!scene.project(receiver, hit.vertex.position, reference, count, root)) return false;
    if (!solveSmsManifold(scene, receiver, light, root, count, 32u, 1.0e-5,
        resolution,smsPrimaryTile(u.xy,resolution))) return false;
    float3 edge = light - root[count-1u].position;
    float distance = length(edge);
    if (!(distance > kRayEpsilon + kMediumEpsilon)) return false;
    return !scene.occluded(root[count-1u].position, edge / distance, distance-kRayEpsilon);
}

bool contribution(SmsWorkRecord work, SmsManifoldVertex root[2], uint count, out float3 value) {
    value = SMS_CONTRIBUTION_ZERO;
    float3 prefix=asfloat(work.words[9].xyz);
    if (!all(isfinite(prefix)) || any(prefix<0.0.xxx)) return false;
    float3 receiver = smsReceiverPosition(work), light = smsLightPosition(work);
    float3 lightNormal = asfloat(work.words[6].xyz);
    if (dot(lightNormal, root[count-1u].position - light) <= 0.0) return true;
    GpuMaterial receiverMat = SMS_MATERIALS[work.words[0].w];
    SMS_TEXTURE_SAMPLER tex;
    float3 wo = asfloat(work.words[4].xyz);
    SurfaceFrame f = applyOpenPbrTextureMaps(receiverMat, tex, wo, asfloat(work.words[1].xyz),
        asfloat(work.words[2].xyz), asfloat(work.words[3].xyz),
        float2(asfloat(work.words[2].w), asfloat(work.words[3].w)));
    float3 wi = normalize(root[0].position - receiver);
    float ignored;
    float3 base = evalBSDF(receiverMat, wo, wi, f.normal, f.T, f.B, f.coatN, f.coatT, f.coatB, f.backface) *
        abs(dot(f.normal, wi)) * shadowTerminatorFactor(f.normal, f.geoNormal, wi, ignored);
    if (all(base == 0.0.xxx)) return true;
    float3 response = 1.0.xxx;
    for (uint i = 0u; i < count; ++i) {
        float3 previous = i == 0u ? receiver : root[i-1u].position;
        float3 next = i+1u == count ? light : root[i+1u].position;
        float3 woS = normalize(previous-root[i].position), wiS = normalize(next-root[i].position);
        float3 n = root[i].normal, ng = root[i].geoNormal;
        if (dot(n, woS) < 0.0) n = -n;
        if (dot(ng, woS) < 0.0) ng = -ng;
        GpuMaterial mat = SMS_MATERIALS[root[i].objectKey.y];
        response *= smsDeltaResponse(mat, woS, n, root[i].event, root[i].etaTOverI);
        response *= shadowTerminatorFactor(n, ng, wiS, ignored);
    }
    if (count == 2u) {
        GpuMaterial mat = SMS_MATERIALS[root[0].objectKey.y];
        if (smsPureAbsorption(mat)) {
            float3 sigmaT, albedo;
            transmissionVolumeCoeffs(mat.transmissionColorWeight.xyz,mat.transmissionScatter.xyz,
                mat.transmissionParams.x,sigmaT,albedo);
            mediumBoundaryTransmittance(response,sigmaT,true,length(root[1].position-root[0].position));
        }
    }
    if (all(response == 0.0.xxx)) return true;
    float3x3 lightFrame = buildFrameFromZ(lightNormal);
    float geometric;
    if (!smsGeometricTerm(receiver, root, count, light, lightFrame[0], lightFrame[1], geometric)) return false;
    float pdf = asfloat(work.words[5].w);
    if (!(pdf > 0.0) || !isfinite(pdf)) return false;
    float3 Le = float3(asfloat(work.words[6].w), asfloat(work.words[7].xy));
    SMS_FINISH_CONTRIBUTION(base,response,geometric,Le,pdf,prefix,value)
}

#ifndef SMS_HELPERS_ONLY
#ifdef SMS_BUILD_PRIOR
[shader("compute")]
[numthreads(128,1,1)]
void main(uint3 group : SV_GroupID,uint lane : SV_GroupIndex) {
    uint2 dimensions=pc.work[0].words[15].zw;
    uint samples=pc.work[0].words[14].y;
    if (samples==0u || any(dimensions==0u.xx)) { smsQueueCompletionError(pc,kSmsQueueInvalidWork); return; }
    uint blocksX=dimensions.x/16u+(dimensions.x%16u!=0u?1u:0u);
    uint block=group.y*blocksX+group.x;
    uint hash=hashCombine(block,pc.work[0].words[14].z^pc.work[0].words[14].w);
    uint permutation=(lane*73u+hash)&255u;
    uint2 pixel=group.xy*16u+uint2(permutation%16u,permutation/16u);
    if (any(pixel>=dimensions)) return;
    uint sample=hashCombine(lane,hash)%samples;
    uint slot=(pixel.y*dimensions.x+pixel.x)*samples+sample;
    if (slot>=pc.capacity) { smsQueueCompletionError(pc,kSmsQueueInvalidWork); return; }
    SmsWorkRecord work=pc.work[slot];
    if (smsPhase(work)!=kSmsInitial) return;
    float4 u=smsTrialSample(work.words[7].zw,uint2(lane,block),uint2(0xFFFFFFFDu,0));
    SMS_SCENE scene;
    SmsManifoldVertex root[2]; uint count;
    if (!samplePath(scene,smsReceiverPosition(work),smsLightPosition(work),u,work.words[13].w,1u,root,count)) return;
    uint n=work.words[12].w;
    if (n==0u || n>32u) { smsQueueCompletionError(pc,kSmsQueueInvalidWork); return; }
    uint2 tile=smsDirectionTile(normalize(root[0].position-smsReceiverPosition(work)),n);
    uint index=tile.y*n+tile.x,key=work.words[13].z+1u,start=smsPriorHash(key);
    SmsPriorEntry* table=(SmsPriorEntry*)pc.userData;
    for (uint offset=0u;offset<kSmsPriorSlots;++offset) {
        uint entry=block*kSmsPriorSlots+(start+offset)%kSmsPriorSlots,observed;
        InterlockedCompareExchange(table[entry].key,0u,key,observed);
        if (observed==0u || observed==key) {
            uint ignored;
            InterlockedOr(table[entry].mask[index/32u],1u<<(index%32u),ignored);
            return;
        }
    }
    smsQueueCompletionError(pc,kSmsQueueOverflow);
}
#else
SmsPriorEntry framePrior(SmsWorkRecord work,uint slot) {
    SmsPriorEntry empty={};
    if (pc.userFlags==0u) return empty;
    uint samples=work.words[14].y,width=work.words[15].z;
    uint pixel=slot/samples,blocksX=width/16u+(width%16u!=0u?1u:0u);
    uint block=(pixel/width/16u)*blocksX+(pixel%width/16u);
    return smsFindPrior((SmsPriorEntry*)pc.userData,block,work.words[13].z);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    uint slot;
    if (!smsQueueSlot(pc, group, lane, slot)) return;
    SmsWorkRecord work = pc.work[slot];
    uint phase = smsPhase(work);
    uint resolution=work.words[12].w;
    if (phase!=kSmsAbsent && (resolution==0u || resolution>32u)) {
        smsQueueError(pc,kSmsQueueInvalidWork); return;
    }
    if (pc.inputHeader->mode == 0u) {
        if (phase == kSmsAbsent) return;
        if (phase != kSmsPrepare) { smsQueueError(pc, kSmsQueueInvalidWork); return; }
        bool prepared = prepareLight(work);
        smsSetPhase(work, prepared ? kSmsInitial : kSmsDone);
        pc.work[slot] = work;
        if (prepared) smsQueueAppend(pc, slot);
        return;
    }
    if (phase != kSmsInitial && phase != kSmsRepeat) { smsQueueError(pc, kSmsQueueInvalidWork); return; }
    SMS_SCENE scene;
    float3 receiver = smsReceiverPosition(work), light = smsLightPosition(work);
    SmsPriorEntry prior=framePrior(work,slot);
    SmsReciprocal reciprocal;
    reciprocal.trials = work.words[12].xy;
    reciprocal.status = work.words[12].z;
    for (uint batchTrial = 0u; batchTrial < pc.trialBudget; ++batchTrial) {
        uint2 trial = phase == kSmsInitial ? uint2(0,0) : reciprocal.trials;
        if (phase != kSmsInitial) {
            ++trial.x;
            if (trial.x == 0u) ++trial.y;
            if (all(trial == uint2(0,0))) { smsQueueError(pc, kSmsQueueNumericError); return; }
        }
        float4 u = smsTrialSample(work.words[7].zw, trial, uint2(0,0));
        uint2 origin=uint2(0,0);
        uint2 extent=resolution.xx;
        if (phase!=kSmsInitial) {
            origin=work.words[14].zw;
            extent=work.words[15].xy;
        }
        SmsConditionalRegion seedRegion;
        seedRegion.origin=origin; seedRegion.extent=extent;
        seedRegion.probability=float(extent.x*extent.y)/float(resolution*resolution);
        float2 seed; float sampledMass;
        if (!smsSamplePrior(u,prior,seedRegion,resolution,seed,sampledMass)) {
            smsQueueError(pc,kSmsQueueNumericError); return;
        }
        u.xy=seed;
        SmsManifoldVertex root[2];
        uint count;
        // ONE common call site, same compute context, same frozen endpoint loads.
        bool solved = samplePath(scene,receiver,light,u,work.words[13].w,resolution,root,count);
        if (phase == kSmsInitial) {
            if (!solved) { phase = kSmsDone; break; }
            float3 base;
            if (!contribution(work, root, count, base)) { smsQueueError(pc, kSmsQueueNumericError); return; }
            work.words[11].xyz = asuint(base);
            SMS_STORE_LOCAL_TARGET(slot,work,base);
            if (all(base == 0.0.xxx)) { phase = kSmsDone; break; }
            smsStoreRoot(work, root, count);
            SmsConditionalRegion conditional=makeSmsConditionalRegion(normalize(root[0].position-receiver),resolution);
            work.words[11].w=asuint(smsPriorRegionProbabilities(prior,conditional,resolution).x);
            work.words[14].zw=conditional.origin;
            work.words[15].xy=conditional.extent;
            reciprocal = beginSmsReciprocal();
            phase = kSmsRepeat;
        } else {
            SmsManifoldVertex initial[2]; uint initialCount;
            smsLoadRoot(work, initial, initialCount);
            bool same = solved && sameSmsRoot(receiver, initial, initialCount, root, count, kSmsRootDirectionTolerance);
            observeSmsReciprocal(reciprocal, same);
            if (reciprocal.status == 2u) { smsQueueError(pc, kSmsQueueNumericError); return; }
            if (reciprocal.status == 1u) {
                float weight;
                if (!completedSmsReciprocal(reciprocal, weight)) { smsQueueError(pc, kSmsQueueNumericError); return; }
                SMS_DIAG_COMPLETE(reciprocal,asfloat(work.words[11].w),weight)
                weight=smsConditionalWeight(weight,asfloat(work.words[11].w));
                float3 estimate = asfloat(work.words[11].xyz) * weight;
                if (!all(isfinite(estimate))) { smsQueueError(pc, kSmsQueueNumericError); return; }
                work.words[11].xyz = asuint(estimate);
                phase = kSmsDone;
                break;
            }
        }
    }
    work.words[12].xyz = uint3(reciprocal.trials, reciprocal.status);
    smsSetPhase(work, phase);
    pc.work[slot] = work;
    if (phase != kSmsDone) smsQueueAppend(pc, slot);
}
#endif
#endif
