// Gpu/compute_d3d11.cpp ── ComputeDevice の中身（COM をここに閉じ込める）
//   ヘッダ側の注記を参照。ここは配線だけで、音の話は一切入っていない。
#include "Gpu/compute_d3d11.h"

#include <d3d11.h>
#include <d3dcompiler.h>

#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace acoustic {
namespace gpu {

namespace {
template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
}  // namespace

struct ComputeDevice::Impl {
    ID3D11Device*         dev = nullptr;
    ID3D11DeviceContext*  ctx = nullptr;
    ID3D11ComputeShader*  cs = nullptr;
    ID3D11Buffer*         cb = nullptr;
    ID3D11Buffer*         in[8] = {};
    ID3D11ShaderResourceView* srv[8] = {};
    ID3D11Buffer*         out = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11Buffer*         staging = nullptr;
    std::size_t           outBytes = 0;
    std::size_t           outStride = 0;
    std::string           err;
    std::string           adapter;

    ~Impl() {
        for (int i = 0; i < 8; ++i) { release(srv[i]); release(in[i]); }
        release(uav); release(out); release(staging); release(cb); release(cs); release(ctx); release(dev);
    }

    bool makeStructured(ID3D11Buffer*& buf, ID3D11ShaderResourceView*& view,
                        const void* data, std::size_t bytes, std::size_t stride) {
        release(view); release(buf);
        if (bytes == 0 || stride == 0) return false;
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = static_cast<UINT>(bytes);
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        bd.StructureByteStride = static_cast<UINT>(stride);
        D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = data;
        if (FAILED(dev->CreateBuffer(&bd, &sd, &buf))) { err = "CreateBuffer(input) に失敗"; return false; }
        D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = DXGI_FORMAT_UNKNOWN;
        vd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        vd.Buffer.FirstElement = 0;
        vd.Buffer.NumElements = static_cast<UINT>(bytes / stride);
        if (FAILED(dev->CreateShaderResourceView(buf, &vd, &view))) { err = "CreateShaderResourceView に失敗"; return false; }
        return true;
    }
};

ComputeDevice::ComputeDevice() : d_(new Impl()) {
    UINT flags = 0;
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   want, 1, D3D11_SDK_VERSION, &d_->dev, &got, &d_->ctx);
    if (FAILED(hr)) {
        // ハードウェアが無い機械でも道を確かめられるよう、WARP（ソフトウェア）へ落ちる。
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                               want, 1, D3D11_SDK_VERSION, &d_->dev, &got, &d_->ctx);
        if (SUCCEEDED(hr)) d_->adapter = "WARP（ソフトウェア）";
    }
    if (FAILED(hr)) { d_->err = "D3D11CreateDevice に失敗（機能レベル 11_0 が無い）"; return; }
    if (d_->adapter.empty()) {
        IDXGIDevice* dxgi = nullptr;
        if (SUCCEEDED(d_->dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgi)))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(dxgi->GetAdapter(&ad))) {
                DXGI_ADAPTER_DESC de{};
                if (SUCCEEDED(ad->GetDesc(&de))) {
                    char name[128] = {};
                    WideCharToMultiByte(CP_UTF8, 0, de.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                    d_->adapter = name;
                }
                ad->Release();
            }
            dxgi->Release();
        }
    }
}

ComputeDevice::~ComputeDevice() { delete d_; }

bool ComputeDevice::available() const { return d_ && d_->dev != nullptr && d_->ctx != nullptr; }
const std::string& ComputeDevice::error() const { return d_->err; }
const std::string& ComputeDevice::adapterName() const { return d_->adapter; }

bool ComputeDevice::setShader(const char* hlsl, const char* entry) {
    if (!available()) return false;
    release(d_->cs);
    ID3DBlob* code = nullptr; ID3DBlob* msg = nullptr;
    const UINT f = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    HRESULT hr = D3DCompile(hlsl, std::strlen(hlsl), "af_trace", nullptr, nullptr, entry, "cs_5_0", f, 0, &code, &msg);
    if (FAILED(hr)) {
        d_->err = "HLSL のコンパイルに失敗: ";
        if (msg) { d_->err.append(static_cast<const char*>(msg->GetBufferPointer()), msg->GetBufferSize()); msg->Release(); }
        if (code) code->Release();
        return false;
    }
    if (msg) msg->Release();
    hr = d_->dev->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &d_->cs);
    code->Release();
    if (FAILED(hr)) { d_->err = "CreateComputeShader に失敗"; return false; }
    return true;
}

bool ComputeDevice::setInput(int slot, const void* data, std::size_t bytes, std::size_t stride) {
    if (!available() || slot < 0 || slot >= 8) return false;
    return d_->makeStructured(d_->in[slot], d_->srv[slot], data, bytes, stride);
}

bool ComputeDevice::setConstants(const void* data, std::size_t bytes) {
    if (!available()) return false;
    release(d_->cb);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>((bytes + 15) & ~std::size_t(15));   // 16 バイト境界
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    std::vector<unsigned char> pad(bd.ByteWidth, 0);
    std::memcpy(pad.data(), data, bytes);
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = pad.data();
    if (FAILED(d_->dev->CreateBuffer(&bd, &sd, &d_->cb))) { d_->err = "CreateBuffer(constants) に失敗"; return false; }
    return true;
}

bool ComputeDevice::setOutput(std::size_t bytes, std::size_t stride) {
    if (!available() || bytes == 0 || stride == 0) return false;
    // ★大きさが同じなら作り直さない。束ねると 1 回が 20 MB を超えるので、
    //   毎フレーム作り直すと確保と解放だけで数 ms 掛かる（音源が動いても大きさは変わらない）。
    if (d_->out && d_->uav && d_->staging && d_->outBytes == bytes && d_->outStride == stride) return true;
    release(d_->uav); release(d_->out); release(d_->staging);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = static_cast<UINT>(bytes);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = static_cast<UINT>(stride);
    if (FAILED(d_->dev->CreateBuffer(&bd, nullptr, &d_->out))) { d_->err = "CreateBuffer(output) に失敗"; return false; }
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = static_cast<UINT>(bytes / stride);
    if (FAILED(d_->dev->CreateUnorderedAccessView(d_->out, &ud, &d_->uav))) { d_->err = "CreateUnorderedAccessView に失敗"; return false; }
    // 読み戻し用（GPU のバッファは直接 Map できない。必ずこれを挟む）
    D3D11_BUFFER_DESC sdsc = bd;
    sdsc.Usage = D3D11_USAGE_STAGING;
    sdsc.BindFlags = 0;
    sdsc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(d_->dev->CreateBuffer(&sdsc, nullptr, &d_->staging))) { d_->err = "CreateBuffer(staging) に失敗"; return false; }
    d_->outBytes = bytes;
    d_->outStride = stride;
    return true;
}

bool ComputeDevice::dispatch(int groupsX) {
    if (!available() || !d_->cs || !d_->uav || groupsX <= 0) return false;
    d_->ctx->CSSetShader(d_->cs, nullptr, 0);
    ID3D11ShaderResourceView* views[8] = {d_->srv[0], d_->srv[1], d_->srv[2], d_->srv[3],
                                          d_->srv[4], d_->srv[5], d_->srv[6], d_->srv[7]};
    d_->ctx->CSSetShaderResources(0, 8, views);
    if (d_->cb) d_->ctx->CSSetConstantBuffers(0, 1, &d_->cb);
    UINT init = 0;
    d_->ctx->CSSetUnorderedAccessViews(0, 1, &d_->uav, &init);
    d_->ctx->Dispatch(static_cast<UINT>(groupsX), 1, 1);
    // 後始末（次の呼び出しで別の物を挿すため）
    ID3D11UnorderedAccessView* nullUav = nullptr;
    d_->ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, &init);
    return true;
}

bool ComputeDevice::readOutput(void* dst, std::size_t bytes) {
    if (!available() || !d_->staging || !d_->out || bytes > d_->outBytes) return false;
    d_->ctx->CopyResource(d_->staging, d_->out);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(d_->ctx->Map(d_->staging, 0, D3D11_MAP_READ, 0, &m))) { d_->err = "Map(staging) に失敗"; return false; }
    std::memcpy(dst, m.pData, bytes);
    d_->ctx->Unmap(d_->staging, 0);
    return true;
}

}  // namespace gpu
}  // namespace acoustic
