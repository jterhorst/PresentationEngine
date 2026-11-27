// slang_msl: compile one Slang entry point to Metal Shading Language, offline.
//
//   slang_msl <input.slang> <entry> <output.metal> [-I <dir>]...
//
// The engine compiles every shader offline (architecture.md, "Tech stack").
// Metal source is compiled to a library when the engine starts, so the build
// doesn't need Xcode's separate Metal toolchain download.
//
// SPDX-License-Identifier: MPL-2.0
#include <slang.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// slangpy ships slang.h but not slang-com-ptr.h, so hold COM references here.
template <class T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->release(); }
    T** writeRef() { return &p; }
    T* operator->() const { return p; }
    operator T*() const { return p; }
};

static int fail(const char* what, slang::IBlob* diag) {
    std::fprintf(stderr, "slang_msl: %s\n", what);
    if (diag) std::fprintf(stderr, "%s\n", static_cast<const char*>(diag->getBufferPointer()));
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: slang_msl <input.slang> <entry> <output.metal> [-I dir]...\n");
        return 2;
    }
    const std::string input = argv[1], entry = argv[2], output = argv[3];
    std::vector<std::string> includes;
    for (int i = 4; i + 1 < argc; i += 2)
        if (std::string(argv[i]) == "-I") includes.push_back(argv[i + 1]);

    ComPtr<slang::IGlobalSession> global;
    if (SLANG_FAILED(slang::createGlobalSession(global.writeRef()))) return fail("createGlobalSession failed", nullptr);

    slang::TargetDesc target{};
    target.format = SLANG_METAL;
    target.profile = global->findProfile("metal");

    std::vector<const char*> paths;
    for (auto& p : includes) paths.push_back(p.c_str());

    slang::SessionDesc desc{};
    desc.targets = &target;
    desc.targetCount = 1;
    desc.searchPaths = paths.data();
    desc.searchPathCount = static_cast<SlangInt>(paths.size());

    ComPtr<slang::ISession> session;
    if (SLANG_FAILED(global->createSession(desc, session.writeRef()))) return fail("createSession failed", nullptr);

    ComPtr<slang::IBlob> diag;
    slang::IModule* module = session->loadModule(input.c_str(), diag.writeRef());
    if (!module) return fail("loadModule failed", diag);

    ComPtr<slang::IEntryPoint> ep;
    if (SLANG_FAILED(module->findEntryPointByName(entry.c_str(), ep.writeRef())))
        return fail(("no entry point " + entry).c_str(), nullptr);

    slang::IComponentType* parts[] = {module, ep};
    ComPtr<slang::IComponentType> composite;
    if (SLANG_FAILED(session->createCompositeComponentType(parts, 2, composite.writeRef(), diag.writeRef())))
        return fail("compose failed", diag);
    ComPtr<slang::IComponentType> linked;
    if (SLANG_FAILED(composite->link(linked.writeRef(), diag.writeRef()))) return fail("link failed", diag);

    ComPtr<slang::IBlob> code;
    if (SLANG_FAILED(linked->getEntryPointCode(0, 0, code.writeRef(), diag.writeRef())))
        return fail("codegen failed", diag);

    std::ofstream out(output, std::ios::binary);
    out.write(static_cast<const char*>(code->getBufferPointer()), static_cast<std::streamsize>(code->getBufferSize()));
    return out ? 0 : fail("write failed", nullptr);
}
