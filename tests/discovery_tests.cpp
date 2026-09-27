#include "builds/image_discovery.h"
#include "builds/build_registry.h"

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

using namespace RVThereYetHeadTracking::builds;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void TestHash()
{
    std::uint8_t code[] = {0x48, 0x8b, 0x05, 0x20, 0, 0, 0, 0xc3};
    const auto hash = NormalizedCodeHash(code, sizeof(code));
    code[3] = 0x40;
    Require(hash == NormalizedCodeHash(code, sizeof(code)), "RIP relocation changed digest");
    code[1] = 0x8d;
    Require(hash != NormalizedCodeHash(code, sizeof(code)), "opcode change was ignored");
    std::uint8_t call[] = {0xe8, 0x40, 0, 0, 0, 0xc3};
    const auto callHash = NormalizedCodeHash(call, sizeof(call));
    call[1] = 0x50;
    Require(callHash == NormalizedCodeHash(call, sizeof(call)), "external call relocation changed digest");
    std::uint8_t branch[] = {0x74, 0x01, 0x90, 0xc3};
    const auto branchHash = NormalizedCodeHash(branch, sizeof(branch));
    branch[1] = 0;
    Require(branchHash != NormalizedCodeHash(branch, sizeof(branch)), "internal branch change was ignored");
    std::uint8_t field[] = {0x48, 0x8b, 0x41, 0x18, 0xc3};
    const auto fieldHash = NormalizedCodeHash(field, sizeof(field));
    field[3] = 0x20;
    Require(fieldHash != NormalizedCodeHash(field, sizeof(field)), "layout change was ignored");
    const std::uint8_t truncated[] = {0x48, 0x8b};
    Require(NormalizedCodeHash(truncated, sizeof(truncated)) == 0, "truncated instruction accepted");
    Require(DiscoverImage(truncated, sizeof(truncated)).error != nullptr, "truncated PE accepted");
    const std::uint8_t invalid[512]{};
    Require(DiscoverImage(invalid, sizeof(invalid)).error != nullptr, "invalid PE accepted");
}

static void TestImage(const char* path, std::uint32_t builder, std::uint32_t objects, std::uint32_t names)
{
    std::ifstream stream(path, std::ios::binary);
    Require(stream.good(), "cannot open image fixture");
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(stream), {}};
    Require(file.size() >= sizeof(IMAGE_DOS_HEADER), "fixture DOS header truncated");
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    Require(dos->e_lfanew > 0 && static_cast<std::size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) <= file.size(),
        "fixture PE header truncated");
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    Require(nt->OptionalHeader.SizeOfImage < 512 * 1024 * 1024, "fixture image too large");
    std::vector<std::uint8_t> image(nt->OptionalHeader.SizeOfImage);
    Require(nt->OptionalHeader.SizeOfHeaders <= file.size() && nt->OptionalHeader.SizeOfHeaders <= image.size(),
        "invalid fixture headers");
    std::memcpy(image.data(), file.data(), nt->OptionalHeader.SizeOfHeaders);
    const auto* sections = IMAGE_FIRST_SECTION(nt);
    Require(reinterpret_cast<const std::uint8_t*>(sections + nt->FileHeader.NumberOfSections) <= file.data() + file.size(),
        "fixture sections truncated");
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const auto& s = sections[i];
        Require(static_cast<std::uint64_t>(s.PointerToRawData) + s.SizeOfRawData <= file.size() &&
            static_cast<std::uint64_t>(s.VirtualAddress) + s.SizeOfRawData <= image.size(), "invalid fixture section");
        std::memcpy(image.data() + s.VirtualAddress, file.data() + s.PointerToRawData, s.SizeOfRawData);
    }
    const auto found = DiscoverImage(image.data(), image.size());
    if (found.error) throw std::runtime_error(found.error);
    Require(found.viewBuilder == builder && found.objects == objects && found.names == names, "unexpected discovered addresses");
    std::cout << path << ": builder=0x" << std::hex << found.viewBuilder << " objects=0x" << found.objects
        << " names=0x" << found.names << '\n';

    const auto first = image[builder];
    image[builder] ^= 1;
    const auto changed = DiscoverImage(image.data(), image.size());
    Require(changed.error && !changed.viewBuilder && !changed.objects && !changed.names,
        "changed camera code accepted or partial addresses escaped");
    image[builder] = first;

    auto* mappedNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + dos->e_lfanew);
    mappedNt->FileHeader.TimeDateStamp = 1;
    mappedNt->OptionalHeader.CheckSum = 2;
    Require(SelectProfile(reinterpret_cast<HMODULE>(image.data())) == MatchResult::Matched,
        "unknown PE fingerprint did not use discovery");
    Require(std::string(ActiveProfile().Name) == "validated-engine-functions" &&
        ActiveProfile().Offsets.kViewBuilderRva == builder &&
        ActiveProfile().Offsets.UObjectGlobals.kObjObjects == objects &&
        ActiveProfile().Offsets.UObjectGlobals.kFNamePool == names, "wrong discovered profile selected");
    auto& exceptions = mappedNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* functions = reinterpret_cast<RUNTIME_FUNCTION*>(image.data() + exceptions.VirtualAddress);
    const auto count = exceptions.Size / sizeof(RUNTIME_FUNCTION);
    RUNTIME_FUNCTION builderFunction{};
    std::uint32_t decoder = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (functions[i].BeginAddress == builder) builderFunction = functions[i];
        if (functions[i].EndAddress - functions[i].BeginAddress == 0xb9 &&
            NormalizedCodeHash(image.data() + functions[i].BeginAddress, 0xb9) == 0x18510280422b7137ULL) {
            decoder = functions[i].BeginAddress;
        }
    }
    Require(builderFunction.BeginAddress != 0, "fixture builder not in exception table");
    Require(decoder != 0, "fixture decoder not found");
    image[decoder + 0x23] ^= 8;
    Require(DiscoverImage(image.data(), image.size()).error != nullptr, "inconsistent name references accepted");
    image[decoder + 0x23] ^= 8;
    image[decoder] ^= 1;
    Require(DiscoverImage(image.data(), image.size()).error != nullptr, "changed name decoder accepted");
    image[decoder] ^= 1;
    const auto saved = functions[0];
    functions[0] = builderFunction;
    Require(DiscoverImage(image.data(), image.size()).error != nullptr, "ambiguous camera accepted");
    Require(SelectProfile(reinterpret_cast<HMODULE>(image.data())) != MatchResult::Matched,
        "unknown ambiguous build armed tracking");
    functions[0] = saved;
    functions[0].EndAddress = static_cast<DWORD>(image.size() + 1);
    Require(DiscoverImage(image.data(), image.size()).error != nullptr, "out of bounds function accepted");
    functions[0] = saved;
    exceptions.Size -= 1;
    Require(DiscoverImage(image.data(), image.size()).error != nullptr, "truncated exception table accepted");
}

int main(int argc, char** argv)
try
{
    TestHash();
    Require(argc == 1 || argc == 5, "usage: discovery_tests [image builder-rva objects-rva names-rva]");
    if (argc == 5) TestImage(argv[1], static_cast<std::uint32_t>(std::stoul(argv[2], nullptr, 0)),
        static_cast<std::uint32_t>(std::stoul(argv[3], nullptr, 0)), static_cast<std::uint32_t>(std::stoul(argv[4], nullptr, 0)));
    std::cout << "Discovery tests passed\n";
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
