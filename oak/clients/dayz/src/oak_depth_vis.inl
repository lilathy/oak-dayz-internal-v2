// Visibility helpers — GPU depth hooks disabled (ClearDSV/mid-frame Copy caused AVs or empty buffers).
// Occlusion uses prop-ray against Near/Far/Slow/Item in oak_batch4_esp.inl.

static bool g_OakDepthCpuValid = false;

static bool OakDepth_InstallHook(ID3D11DeviceContext* /*ctx*/)
{
    return false;
}

static void OakDepth_BeginFrame()
{
    g_OakDepthCpuValid = false;
}

static void OakDepth_OnPresent(ID3D11Device* /*dev*/, ID3D11DeviceContext* /*ctx*/)
{
}

static void OakDepth_EndFrameLog()
{
}

static bool OakDepth_IsWorldOccluded(const Vec3& /*world*/, float* outViewZ)
{
    if (outViewZ) *outViewZ = 0.f;
    return false;
}
