#include "image_discovery.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <windows.h>
#include <hde64.h>

namespace RVThereYetHeadTracking::builds
{
    std::uint64_t NormalizedCodeHash(const std::uint8_t* code, std::size_t size)
    {
        std::uint64_t hash = 14695981039346656037ULL;
        for (std::size_t offset = 0; offset < size;) {
            // HDE may inspect beyond an invalid instruction. Pad the final
            // bytes so a truncated function never reads outside its range.
            std::array<std::uint8_t, 32> bytes{};
            std::memcpy(bytes.data(), code + offset, std::min(size - offset, std::size_t{15}));
            hde64s ins{};
            hde64_disasm(bytes.data(), &ins);
            if ((ins.flags & F_ERROR) || ins.len == 0 || ins.len > size - offset) return 0;

            const unsigned immediate = (ins.flags & F_IMM64) ? 8u :
                (ins.flags & F_IMM32) ? 4u : (ins.flags & F_IMM16) ? 2u :
                (ins.flags & F_IMM8) ? 1u : 0u;
            if ((ins.flags & F_MODRM) && ins.modrm_mod == 0 && ins.modrm_rm == 5 && !ins.p_67) {
                std::fill_n(bytes.data() + ins.len - immediate - 4, 4, std::uint8_t{0});
            }
            if (ins.flags & F_RELATIVE) {
                const auto displacement = immediate == 1 ? static_cast<std::int8_t>(ins.imm.imm8) :
                    static_cast<std::int32_t>(ins.imm.imm32);
                const auto target = static_cast<std::int64_t>(offset) + ins.len + displacement;
                // Internal branches describe control flow. Only destinations
                // outside the function are allowed to move with a relink.
                if (target < 0 || target >= static_cast<std::int64_t>(size)) {
                    std::fill_n(bytes.data() + ins.len - immediate, immediate, std::uint8_t{0});
                }
            }
            for (unsigned i = 0; i < ins.len; ++i) hash = (hash ^ bytes[i]) * 1099511628211ULL;
            offset += ins.len;
        }
        return hash;
    }

    DiscoveredAddresses DiscoverImage(const std::uint8_t* image, std::size_t size)
    {
        const auto failure = [](const char* error) { return DiscoveredAddresses{0, 0, 0, error}; };
        const auto contains = [size](std::size_t offset, std::size_t length) {
            return offset <= size && length <= size - offset;
        };
        if (!contains(0, sizeof(IMAGE_DOS_HEADER))) return failure("truncated DOS header");
        IMAGE_DOS_HEADER dos{};
        std::memcpy(&dos, image, sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
            !contains(static_cast<std::size_t>(dos.e_lfanew), sizeof(IMAGE_NT_HEADERS64))) {
            return failure("invalid PE header");
        }
        IMAGE_NT_HEADERS64 nt{};
        std::memcpy(&nt, image + dos.e_lfanew, sizeof(nt));
        if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            nt.FileHeader.SizeOfOptionalHeader != sizeof(IMAGE_OPTIONAL_HEADER64) ||
            nt.OptionalHeader.SizeOfImage != size ||
            nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION) {
            return failure("unsupported PE layout");
        }
        const auto sectionOffset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(nt);
        if (!contains(sectionOffset, nt.FileHeader.NumberOfSections * sizeof(IMAGE_SECTION_HEADER))) {
            return failure("invalid section table");
        }
        const auto inSection = [&](std::uint32_t rva, std::size_t length, DWORD required, DWORD forbidden) {
            if (!contains(rva, length)) return false;
            for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
                IMAGE_SECTION_HEADER section{};
                std::memcpy(&section, image + sectionOffset + i * sizeof(section), sizeof(section));
                if ((section.Characteristics & required) != required || (section.Characteristics & forbidden)) continue;
                if (rva >= section.VirtualAddress && rva - section.VirtualAddress <= section.Misc.VirtualSize &&
                    length <= section.Misc.VirtualSize - (rva - section.VirtualAddress)) return true;
            }
            return false;
        };
        const auto& exception = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!exception.Size || exception.Size % sizeof(RUNTIME_FUNCTION) != 0 ||
            !inSection(exception.VirtualAddress, exception.Size, IMAGE_SCN_MEM_READ, IMAGE_SCN_MEM_EXECUTE)) {
            return failure("invalid exception table");
        }

        struct Signature { std::uint32_t length; std::uint64_t hash; };
        // These are digests, not copies of game instructions. Every byte other
        // than a relocation-dependent operand participates, including layouts,
        // virtual slots and internal branch destinations.
        constexpr std::array<Signature, 3> signatures{{
            {0x30b, 0x43bb7b70c1984478ULL},
            {0x2a3, 0xd7d6efde59a08338ULL},
            {0x0b9, 0x18510280422b7137ULL},
        }};
        std::array<std::uint32_t, 3> matches{};
        std::array<unsigned, 3> counts{};
        for (std::size_t offset = 0; offset < exception.Size; offset += sizeof(RUNTIME_FUNCTION)) {
            RUNTIME_FUNCTION function{};
            std::memcpy(&function, image + exception.VirtualAddress + offset, sizeof(function));
            if (function.BeginAddress >= function.EndAddress ||
                !inSection(function.BeginAddress, function.EndAddress - function.BeginAddress,
                    IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE, IMAGE_SCN_MEM_WRITE)) {
                return failure("invalid executable function range");
            }
            for (std::size_t i = 0; i < signatures.size(); ++i) {
                if (function.EndAddress - function.BeginAddress == signatures[i].length &&
                    NormalizedCodeHash(image + function.BeginAddress, signatures[i].length) == signatures[i].hash) {
                    matches[i] = function.BeginAddress;
                    ++counts[i];
                }
            }
        }
        if (counts[0] != 1) return failure("camera builder fingerprint missing or ambiguous");
        if (counts[1] != 1) return failure("object allocator fingerprint missing or ambiguous");
        if (counts[2] != 1) return failure("name decoder fingerprint missing or ambiguous");

        const auto ripTarget = [&](std::uint32_t instruction, unsigned length, unsigned displacementOffset) {
            std::int32_t displacement = 0;
            std::memcpy(&displacement, image + instruction + displacementOffset, sizeof(displacement));
            const auto target = static_cast<std::int64_t>(instruction) + length + displacement;
            return target >= 0 && target < static_cast<std::int64_t>(size) ? static_cast<std::uint32_t>(target) : 0u;
        };
        const auto objects = ripTarget(matches[1] + 0x18e, 7, 3);
        const auto names = ripTarget(matches[2] + 0x20, 7, 3);
        const auto namesOnInit = ripTarget(matches[2] + 0x29, 7, 3);
        const auto initFlag = ripTarget(matches[2] + 0x0f, 7, 2);
        if (!objects || !names || objects % 8 || names % 8 || names != namesOnInit ||
            names < initFlag || names - initFlag != 0x267 ||
            ripTarget(matches[2] + 0x38, 7, 2) != initFlag ||
            ripTarget(matches[1] + 0x158, 6, 2) != objects + 0x18 ||
            ripTarget(matches[1] + 0x161, 6, 2) != objects + 0x10 ||
            ripTarget(matches[1] + 0x17e, 7, 3) != objects + 0x18 ||
            ripTarget(matches[1] + 0x1a3, 7, 3) != objects + 0x10 ||
            !inSection(objects, 0x20, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) ||
            !inSection(names, 0x18, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE) ||
            !inSection(initFlag, 1, IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE, IMAGE_SCN_MEM_EXECUTE)) {
            return failure("reflection references disagree or point outside writable data");
        }
        return {matches[0], objects, names, nullptr};
    }
}
