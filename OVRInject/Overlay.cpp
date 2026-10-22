#include "targetver.h"

#include "Overlay.hpp"
#include "Log.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <mutex>
#include <string>

using namespace OVRInject;

// ---- Shared state (pose thread writes, render thread reads) ----

static std::mutex g_overlay_mutex;
static std::string g_overlay_text;
static double g_overlay_deadline = 0.0;
static bool g_overlay_enabled = true;
static float g_overlay_duration = 2.5f;

static double OverlayNow() {
  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / (double)freq.QuadPart;
}

void OVRInject::Overlay_Configure(bool enabled, float duration_seconds) {
  std::lock_guard<std::mutex> lock(g_overlay_mutex);
  g_overlay_enabled = enabled;
  g_overlay_duration = duration_seconds;
}

void OVRInject::Overlay_Notify(const char* text) {
  std::lock_guard<std::mutex> lock(g_overlay_mutex);
  g_overlay_text = text;
  g_overlay_deadline = OverlayNow() + (double)g_overlay_duration;
}

// ---- Render-thread D3D state (game device) ----

static ID3D11Device* g_device = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_ps = nullptr;
static ID3D11InputLayout* g_layout = nullptr;
static ID3D11Buffer* g_vb = nullptr;
static ID3D11SamplerState* g_sampler = nullptr;
static ID3D11BlendState* g_blend = nullptr;

static ID3D11Texture2D* g_text_texture = nullptr;
static ID3D11ShaderResourceView* g_text_srv = nullptr;
static int g_text_width = 0;
static int g_text_height = 0;
static std::string g_baked_text;

static ID3D11Texture2D* g_cached_backbuffer = nullptr;
static ID3D11RenderTargetView* g_cached_rtv = nullptr;

static bool g_init_failed = false;

static const char* OVERLAY_HLSL =
"struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };\n"
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VSOut vsmain(VSIn i) {\n"
"  VSOut o; o.pos = float4(i.pos, 0.0, 1.0); o.uv = i.uv; return o;\n"
"}\n"
"Texture2D tex0 : register(t0);\n"
"SamplerState samp0 : register(s0);\n"
"float4 psmain(VSOut i) : SV_TARGET {\n"
"  float t = tex0.Sample(samp0, i.uv).r;\n"
"  float a = max(t, 0.55);\n"
"  return float4(t, t, t, a);\n"
"}\n";

struct OverlayVertex {
  float x, y, u, v;
};

static bool OverlayInitPipeline(ID3D11Device* device) {
  ID3DBlob* vs_blob = nullptr;
  ID3DBlob* ps_blob = nullptr;
  ID3DBlob* errors = nullptr;

  HRESULT hr = D3DCompile(OVERLAY_HLSL, strlen(OVERLAY_HLSL), "overlay",
                          nullptr, nullptr, "vsmain", "vs_4_0", 0, 0,
                          &vs_blob, &errors);
  if (FAILED(hr)) {
    LOGSTRF("Overlay: VS compile failed 0x%X\n", hr);
    if (errors) errors->Release();
    return false;
  }

  hr = D3DCompile(OVERLAY_HLSL, strlen(OVERLAY_HLSL), "overlay",
                  nullptr, nullptr, "psmain", "ps_4_0", 0, 0,
                  &ps_blob, &errors);
  if (FAILED(hr)) {
    LOGSTRF("Overlay: PS compile failed 0x%X\n", hr);
    vs_blob->Release();
    if (errors) errors->Release();
    return false;
  }

  device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_vs);
  device->CreatePixelShader(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &g_ps);

  D3D11_INPUT_ELEMENT_DESC elems[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
  };
  device->CreateInputLayout(elems, 2, vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), &g_layout);

  vs_blob->Release();
  ps_blob->Release();

  D3D11_BUFFER_DESC vb_desc = { 0 };
  vb_desc.ByteWidth = sizeof(OverlayVertex) * 4;
  vb_desc.Usage = D3D11_USAGE_DYNAMIC;
  vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  device->CreateBuffer(&vb_desc, nullptr, &g_vb);

  D3D11_SAMPLER_DESC samp_desc = {};
  samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
  device->CreateSamplerState(&samp_desc, &g_sampler);

  D3D11_BLEND_DESC blend_desc = {};
  blend_desc.RenderTarget[0].BlendEnable = TRUE;
  blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
  blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
  blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
  blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  device->CreateBlendState(&blend_desc, &g_blend);

  return g_vs && g_ps && g_layout && g_vb && g_sampler && g_blend;
}

// Renders `text` into a 32bpp GDI bitmap (white on black) and uploads
// it as an immutable texture. Returns false on failure.
static bool OverlayBakeText(ID3D11Device* device, const std::string& text) {
  if (g_text_srv) { g_text_srv->Release(); g_text_srv = nullptr; }
  if (g_text_texture) { g_text_texture->Release(); g_text_texture = nullptr; }

  HDC screen_dc = GetDC(NULL);
  HDC mem_dc = CreateCompatibleDC(screen_dc);
  ReleaseDC(NULL, screen_dc);

  HFONT font = CreateFontA(-26, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");
  HFONT old_font = (HFONT)SelectObject(mem_dc, font);

  SIZE extent = { 0 };
  GetTextExtentPoint32A(mem_dc, text.c_str(), (int)text.length(), &extent);

  const int pad = 10;
  int width = extent.cx + pad * 2;
  int height = extent.cy + pad * 2;

  BITMAPINFO bmi = {};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  void* bits = nullptr;
  HBITMAP bitmap = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
  if (bitmap == NULL || bits == nullptr) {
    SelectObject(mem_dc, old_font);
    DeleteObject(font);
    DeleteDC(mem_dc);
    return false;
  }

  HBITMAP old_bitmap = (HBITMAP)SelectObject(mem_dc, bitmap);

  RECT rect = { 0, 0, width, height };
  HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
  FillRect(mem_dc, &rect, black);

  SetTextColor(mem_dc, RGB(255, 255, 255));
  SetBkMode(mem_dc, TRANSPARENT);
  TextOutA(mem_dc, pad, pad, text.c_str(), (int)text.length());
  GdiFlush();

  D3D11_TEXTURE2D_DESC tex_desc = {};
  tex_desc.Width = width;
  tex_desc.Height = height;
  tex_desc.MipLevels = 1;
  tex_desc.ArraySize = 1;
  tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;
  tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
  tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

  D3D11_SUBRESOURCE_DATA init = {};
  init.pSysMem = bits;
  init.SysMemPitch = width * 4;

  HRESULT hr = device->CreateTexture2D(&tex_desc, &init, &g_text_texture);

  SelectObject(mem_dc, old_bitmap);
  SelectObject(mem_dc, old_font);
  DeleteObject(bitmap);
  DeleteObject(font);
  DeleteDC(mem_dc);

  if (FAILED(hr)) {
    LOGSTRF("Overlay: text texture creation failed 0x%X\n", hr);
    return false;
  }

  hr = device->CreateShaderResourceView(g_text_texture, nullptr, &g_text_srv);
  if (FAILED(hr)) {
    g_text_texture->Release();
    g_text_texture = nullptr;
    return false;
  }

  g_text_width = width;
  g_text_height = height;
  g_baked_text = text;

  return true;
}

void OVRInject::Overlay_Render(IDXGISwapChain* swap_chain) {
  if (g_init_failed)
    return;

  // Snapshot the shared state
  std::string text;
  {
    std::lock_guard<std::mutex> lock(g_overlay_mutex);
    if (!g_overlay_enabled || g_overlay_text.empty() ||
        OverlayNow() >= g_overlay_deadline)
      return;
    text = g_overlay_text;
  }

  ID3D11Texture2D* backbuffer = nullptr;
  swap_chain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backbuffer);
  if (backbuffer == nullptr)
    return;

  ID3D11Device* device = nullptr;
  backbuffer->GetDevice(&device);
  if (device == nullptr) {
    backbuffer->Release();
    return;
  }

  ID3D11DeviceContext* ctx = nullptr;
  device->GetImmediateContext(&ctx);

  // Lazy init on the game's device
  if (g_device != device) {
    g_device = device;
    if (!OverlayInitPipeline(device)) {
      LOGSTRF("Overlay: pipeline init failed, overlay disabled\n");
      g_init_failed = true;
      ctx->Release(); device->Release(); backbuffer->Release();
      return;
    }
  }

  // (Re)bake the text bitmap when the message changes
  if (text != g_baked_text || g_text_srv == nullptr) {
    if (!OverlayBakeText(device, text)) {
      ctx->Release(); device->Release(); backbuffer->Release();
      return;
    }
  }

  // Cache an RTV for this backbuffer
  if (g_cached_backbuffer != backbuffer) {
    if (g_cached_rtv) { g_cached_rtv->Release(); g_cached_rtv = nullptr; }
    if (SUCCEEDED(device->CreateRenderTargetView(backbuffer, nullptr, &g_cached_rtv)))
      g_cached_backbuffer = backbuffer;
    else {
      ctx->Release(); device->Release(); backbuffer->Release();
      return;
    }
  }

  D3D11_TEXTURE2D_DESC bb_desc;
  backbuffer->GetDesc(&bb_desc);

  // Quad rect in pixels (top-left corner), converted to NDC
  float x0 = 16.0f, y0 = 16.0f;
  float x1 = x0 + (float)g_text_width;
  float y1 = y0 + (float)g_text_height;

  float w = (float)bb_desc.Width;
  float h = (float)bb_desc.Height;

  auto to_ndc_x = [w](float px) { return px / w * 2.0f - 1.0f; };
  auto to_ndc_y = [h](float py) { return 1.0f - py / h * 2.0f; };

  OverlayVertex verts[4] = {
    { to_ndc_x(x0), to_ndc_y(y0), 0.0f, 0.0f },
    { to_ndc_x(x1), to_ndc_y(y0), 1.0f, 0.0f },
    { to_ndc_x(x0), to_ndc_y(y1), 0.0f, 1.0f },
    { to_ndc_x(x1), to_ndc_y(y1), 1.0f, 1.0f },
  };

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(ctx->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
    memcpy(mapped.pData, verts, sizeof(verts));
    ctx->Unmap(g_vb, 0);
  }

  // ---- Save the game's pipeline state we are about to touch ----
  ID3D11RenderTargetView* old_rtv = nullptr;
  ID3D11DepthStencilView* old_dsv = nullptr;
  ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);

  UINT old_vp_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
  D3D11_VIEWPORT old_vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
  ctx->RSGetViewports(&old_vp_count, old_vps);

  ID3D11RasterizerState* old_rs = nullptr;
  ctx->RSGetState(&old_rs);

  ID3D11BlendState* old_blend = nullptr;
  FLOAT old_blend_factor[4];
  UINT old_blend_mask = 0;
  ctx->OMGetBlendState(&old_blend, old_blend_factor, &old_blend_mask);

  ID3D11DepthStencilState* old_dss = nullptr;
  UINT old_stencil_ref = 0;
  ctx->OMGetDepthStencilState(&old_dss, &old_stencil_ref);

  ID3D11InputLayout* old_layout = nullptr;
  ctx->IAGetInputLayout(&old_layout);

  ID3D11Buffer* old_vb = nullptr;
  UINT old_stride = 0, old_offset = 0;
  ctx->IAGetVertexBuffers(0, 1, &old_vb, &old_stride, &old_offset);

  D3D11_PRIMITIVE_TOPOLOGY old_topology;
  ctx->IAGetPrimitiveTopology(&old_topology);

  ID3D11VertexShader* old_vs = nullptr;
  ID3D11PixelShader* old_ps = nullptr;
  ctx->VSGetShader(&old_vs, nullptr, nullptr);
  ctx->PSGetShader(&old_ps, nullptr, nullptr);

  ID3D11ShaderResourceView* old_srv = nullptr;
  ctx->PSGetShaderResources(0, 1, &old_srv);

  ID3D11SamplerState* old_sampler = nullptr;
  ctx->PSGetSamplers(0, 1, &old_sampler);

  // ---- Draw the overlay quad ----
  ctx->OMSetRenderTargets(1, &g_cached_rtv, nullptr);

  D3D11_VIEWPORT vp = { 0 };
  vp.Width = w;
  vp.Height = h;
  vp.MaxDepth = 1.0f;
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(nullptr);

  FLOAT blend_factor[4] = { 0, 0, 0, 0 };
  ctx->OMSetBlendState(g_blend, blend_factor, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);

  ctx->IASetInputLayout(g_layout);
  UINT stride = sizeof(OverlayVertex), offset = 0;
  ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

  ctx->VSSetShader(g_vs, nullptr, 0);
  ctx->PSSetShader(g_ps, nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &g_text_srv);
  ctx->PSSetSamplers(0, 1, &g_sampler);

  ctx->Draw(4, 0);

  // ---- Restore the game's pipeline state ----
  ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);
  ctx->RSSetViewports(old_vp_count, old_vps);
  ctx->RSSetState(old_rs);
  ctx->OMSetBlendState(old_blend, old_blend_factor, old_blend_mask);
  ctx->OMSetDepthStencilState(old_dss, old_stencil_ref);
  ctx->IASetInputLayout(old_layout);
  ctx->IASetVertexBuffers(0, 1, &old_vb, &old_stride, &old_offset);
  ctx->IASetPrimitiveTopology(old_topology);
  ctx->VSSetShader(old_vs, nullptr, 0);
  ctx->PSSetShader(old_ps, nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &old_srv);
  ctx->PSSetSamplers(0, 1, &old_sampler);

  if (old_rtv) old_rtv->Release();
  if (old_dsv) old_dsv->Release();
  if (old_rs) old_rs->Release();
  if (old_blend) old_blend->Release();
  if (old_dss) old_dss->Release();
  if (old_layout) old_layout->Release();
  if (old_vb) old_vb->Release();
  if (old_vs) old_vs->Release();
  if (old_ps) old_ps->Release();
  if (old_srv) old_srv->Release();
  if (old_sampler) old_sampler->Release();

  ctx->Release();
  device->Release();
  backbuffer->Release();
}
