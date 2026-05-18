#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "dng_auto_ptr.h"
#include "dng_big_table.h"
#include "dng_camera_profile.h"
#include "dng_fingerprint.h"
#include "dng_host.h"
#include "dng_hue_sat_map.h"
#include "dng_memory.h"
#include "dng_xmp.h"
#include "dng_xmp_sdk.h"

namespace {

struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct CubeLut3D {
    int size = 0;
    std::array<double, 3> domainMin {0.0, 0.0, 0.0};
    std::array<double, 3> domainMax {1.0, 1.0, 1.0};
    std::vector<Vec3> data;  // IRIDAS .cube order: R fastest, then G, then B.

    Vec3 Sample(Vec3 in) const {
        if (size < 2) {
            throw std::runtime_error("LUT_3D_SIZE must be at least 2");
        }

        auto normalize = [&](double v, int c) {
            const double d = domainMax[c] - domainMin[c];
            if (std::abs(d) < 1e-12) {
                return 0.0;
            }
            return std::clamp((v - domainMin[c]) / d, 0.0, 1.0);
        };

        const double nr = normalize(in.x, 0);
        const double ng = normalize(in.y, 1);
        const double nb = normalize(in.z, 2);

        const double fr = nr * (size - 1);
        const double fg = ng * (size - 1);
        const double fb = nb * (size - 1);

        const int r0 = static_cast<int>(std::floor(fr));
        const int g0 = static_cast<int>(std::floor(fg));
        const int b0 = static_cast<int>(std::floor(fb));

        const int r1 = std::min(r0 + 1, size - 1);
        const int g1 = std::min(g0 + 1, size - 1);
        const int b1 = std::min(b0 + 1, size - 1);

        const double tr = fr - r0;
        const double tg = fg - g0;
        const double tb = fb - b0;

        auto at = [&](int r, int g, int b) -> const Vec3& {
            const size_t idx = static_cast<size_t>(((b * size) + g) * size + r);
            return data.at(idx);
        };

        auto lerp = [](const Vec3& a, const Vec3& b, double t) {
            return Vec3 {
                a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t,
                a.z + (b.z - a.z) * t,
            };
        };

        const Vec3 c000 = at(r0, g0, b0);
        const Vec3 c100 = at(r1, g0, b0);
        const Vec3 c010 = at(r0, g1, b0);
        const Vec3 c110 = at(r1, g1, b0);
        const Vec3 c001 = at(r0, g0, b1);
        const Vec3 c101 = at(r1, g0, b1);
        const Vec3 c011 = at(r0, g1, b1);
        const Vec3 c111 = at(r1, g1, b1);

        const Vec3 c00 = lerp(c000, c100, tr);
        const Vec3 c10 = lerp(c010, c110, tr);
        const Vec3 c01 = lerp(c001, c101, tr);
        const Vec3 c11 = lerp(c011, c111, tr);

        const Vec3 c0 = lerp(c00, c10, tg);
        const Vec3 c1 = lerp(c01, c11, tg);

        return lerp(c0, c1, tb);
    }
};

std::string Trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

CubeLut3D ParseCube3D(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("Failed to open CUBE file: " + path);
    }

    CubeLut3D lut;
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        ++lineNo;
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0, hash);
        }

        line = Trim(line);
        if (line.empty()) {
            continue;
        }

        std::istringstream iss(line);
        std::string key;
        iss >> key;

        if (key == "TITLE") {
            continue;
        }

        if (key == "LUT_1D_SIZE") {
            throw std::runtime_error("LUT_1D_SIZE found; this converter expects LUT_3D_SIZE");
        }

        if (key == "LUT_3D_SIZE") {
            iss >> lut.size;
            if (lut.size <= 1) {
                throw std::runtime_error("Invalid LUT_3D_SIZE at line " + std::to_string(lineNo));
            }
            continue;
        }

        if (key == "DOMAIN_MIN") {
            if (!(iss >> lut.domainMin[0] >> lut.domainMin[1] >> lut.domainMin[2])) {
                throw std::runtime_error("Invalid DOMAIN_MIN at line " + std::to_string(lineNo));
            }
            continue;
        }

        if (key == "DOMAIN_MAX") {
            if (!(iss >> lut.domainMax[0] >> lut.domainMax[1] >> lut.domainMax[2])) {
                throw std::runtime_error("Invalid DOMAIN_MAX at line " + std::to_string(lineNo));
            }
            continue;
        }

        // Data line
        double r = 0.0, g = 0.0, b = 0.0;
        {
            std::istringstream dataIss(line);
            if (!(dataIss >> r >> g >> b)) {
                throw std::runtime_error("Invalid LUT row at line " + std::to_string(lineNo));
            }
        }
        lut.data.push_back({r, g, b});
    }

    if (lut.size <= 1) {
        throw std::runtime_error("Missing LUT_3D_SIZE in CUBE file");
    }

    const size_t expected = static_cast<size_t>(lut.size) * lut.size * lut.size;
    if (lut.data.size() != expected) {
        std::ostringstream oss;
        oss << "CUBE data count mismatch: expected " << expected
            << ", got " << lut.data.size();
        throw std::runtime_error(oss.str());
    }

    return lut;
}

Vec3 HSVToRGB(double h, double s, double v) {
    h = h - std::floor(h);
    s = std::clamp(s, 0.0, 1.0);
    v = std::clamp(v, 0.0, 1.0);

    if (s <= 1e-12) {
        return {v, v, v};
    }

    const double hh = h * 6.0;
    const int i = static_cast<int>(std::floor(hh)) % 6;
    const double f = hh - std::floor(hh);

    const double p = v * (1.0 - s);
    const double q = v * (1.0 - s * f);
    const double t = v * (1.0 - s * (1.0 - f));

    switch (i) {
        case 0: return {v, t, p};
        case 1: return {q, v, p};
        case 2: return {p, v, t};
        case 3: return {p, q, v};
        case 4: return {t, p, v};
        default: return {v, p, q};
    }
}

Vec3 RGBToHSV(const Vec3& rgb) {
    const double r = rgb.x;
    const double g = rgb.y;
    const double b = rgb.z;

    const double maxv = std::max({r, g, b});
    const double minv = std::min({r, g, b});
    const double c = maxv - minv;

    double h = 0.0;
    if (c > 1e-12) {
        if (maxv == r) {
            h = std::fmod((g - b) / c, 6.0);
        } else if (maxv == g) {
            h = ((b - r) / c) + 2.0;
        } else {
            h = ((r - g) / c) + 4.0;
        }
        h /= 6.0;
        if (h < 0.0) {
            h += 1.0;
        }
    }

    const double s = (maxv <= 1e-12) ? 0.0 : (c / maxv);
    const double v = maxv;

    return {h, s, v};
}

double WrapHueShiftDegrees(double deltaDegrees) {
    while (deltaDegrees > 180.0) {
        deltaDegrees -= 360.0;
    }
    while (deltaDegrees < -180.0) {
        deltaDegrees += 360.0;
    }
    return deltaDegrees;
}

dng_hue_sat_map BuildHueSatMapFromCube(const CubeLut3D& cube,
                                        uint32_t hueDivs,
                                        uint32_t satDivs,
                                        uint32_t valDivs) {
    dng_hue_sat_map map;
    map.SetDivisions(hueDivs, satDivs, valDivs);

    for (uint32_t v = 0; v < valDivs; ++v) {
        const double vv = (valDivs <= 1) ? 0.0 : static_cast<double>(v) / static_cast<double>(valDivs - 1);

        for (uint32_t h = 0; h < hueDivs; ++h) {
            const double hh = (hueDivs <= 1) ? 0.0 : static_cast<double>(h) / static_cast<double>(hueDivs);

            for (uint32_t s = 0; s < satDivs; ++s) {
                const double ss = (satDivs <= 1) ? 0.0 : static_cast<double>(s) / static_cast<double>(satDivs - 1);

                const Vec3 inRGB = HSVToRGB(hh, ss, vv);
                Vec3 outRGB = cube.Sample(inRGB);

                outRGB.x = std::clamp(outRGB.x, 0.0, 1.0);
                outRGB.y = std::clamp(outRGB.y, 0.0, 1.0);
                outRGB.z = std::clamp(outRGB.z, 0.0, 1.0);

                const Vec3 inHSV = RGBToHSV(inRGB);
                const Vec3 outHSV = RGBToHSV(outRGB);

                dng_hue_sat_map::HSBModify m;

                const double hueShift = WrapHueShiftDegrees((outHSV.x - inHSV.x) * 360.0);
                m.fHueShift = static_cast<real32>(hueShift);

                if (inHSV.y <= 1e-6) {
                    m.fSatScale = 1.0f;
                } else {
                    m.fSatScale = static_cast<real32>(std::max(0.0, outHSV.y / inHSV.y));
                }

                if (inHSV.z <= 1e-6) {
                    m.fValScale = 1.0f;
                } else {
                    m.fValScale = static_cast<real32>(std::max(0.0, outHSV.z / inHSV.z));
                }

                map.SetDelta(h, s, v, m);
            }
        }
    }

    return map;
}

std::string FingerprintToHex(const dng_fingerprint& fp) {
    char hex[2 * dng_fingerprint::kDNGFingerprintSize + 1] = {0};
    fp.ToUtf8HexString(hex);
    return std::string(hex);
}

std::string GenerateUuidHex32() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t a = dist(gen);
    uint64_t b = dist(gen);

    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setfill('0')
        << std::setw(16) << a << std::setw(16) << b;
    return oss.str();
}

void SetAltLang(dng_xmp& xmp, const char* ns, const char* path, const std::string& value) {
    dng_string s;
    s.Set(value.c_str());
    xmp.SetAltLangDefault(ns, path, s);
}

void WriteLookXmp(const std::string& outXmpPath,
                  const std::string& profileName,
                  const dng_look_table& lookTable,
                  const std::string& encodedTable,
                  dng_memory_allocator& allocator) {
    dng_xmp xmp(allocator);
    xmp.RequireMeta();

    xmp.Set(XMP_NS_CRS, "PresetType", "Look");
    xmp.Set(XMP_NS_CRS, "Cluster", "");
    xmp.Set(XMP_NS_CRS, "CameraModelRestriction", "");
    xmp.Set(XMP_NS_CRS, "Copyright", "");
    xmp.Set(XMP_NS_CRS, "ContactInfo", "");
    xmp.SetBoolean(XMP_NS_CRS, "SupportsAmount", true);
    xmp.SetBoolean(XMP_NS_CRS, "SupportsColor", true);
    xmp.SetBoolean(XMP_NS_CRS, "SupportsMonochrome", true);
    xmp.SetBoolean(XMP_NS_CRS, "SupportsHighDynamicRange", true);
    xmp.SetBoolean(XMP_NS_CRS, "SupportsNormalDynamicRange", true);
    xmp.SetBoolean(XMP_NS_CRS, "SupportsSceneReferred", true);
    xmp.SetBoolean(XMP_NS_CRS, "SupportsOutputReferred", true);
    xmp.SetBoolean(XMP_NS_CRS, "HasSettings", true);
    xmp.SetBoolean(XMP_NS_CRS, "ConvertToGrayscale", false);
    // ToneMapStrength is required by Lightroom for Look profiles.
    // 0 = no additional tone mapping on top of the LUT.
    xmp.Set(XMP_NS_CRS, "ToneMapStrength", "0");

    xmp.Set(XMP_NS_CRS, "Version", "15.4");
    xmp.Set(XMP_NS_CRS, "ProcessVersion", "11.0");

    SetAltLang(xmp, XMP_NS_CRS, "Name",        profileName);
    SetAltLang(xmp, XMP_NS_CRS, "ShortName",   profileName);
    SetAltLang(xmp, XMP_NS_CRS, "SortName",    profileName);
    SetAltLang(xmp, XMP_NS_CRS, "Group",       "Profiles");
    SetAltLang(xmp, XMP_NS_CRS, "Description", "");

    const std::string uuidHex = GenerateUuidHex32();
    xmp.Set(XMP_NS_CRS, "UUID", uuidHex.c_str());

    const dng_fingerprint& fp = lookTable.Fingerprint();
    xmp.SetFingerprint(XMP_NS_CRS, "LookTable", fp);

    const dng_string encodedFingerprint = dng_xmp::EncodeFingerprint(fp);
    std::string tablePath = "Table_";
    tablePath += encodedFingerprint.Get();

    xmp.Set(XMP_NS_CRS, tablePath.c_str(), encodedTable.c_str());

    // asPacket=false  → no <?xpacket?> wrapper (matches reference profiles)
    // padBytes=0      → no trailing whitespace padding
    // compact=true    → scalar values as RDF attributes, LangAlt as elements
    AutoPtr<dng_memory_block> serialized(xmp.Serialize(false, 0, 0, false, true));

    std::ofstream out(outXmpPath, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to open output XMP path: " + outXmpPath);
    }
    out.write(serialized->Buffer_char(), static_cast<std::streamsize>(serialized->LogicalSize()));
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) {
            std::cerr
                << "Usage: " << argv[0]
                << " <input.cube> <output.xmp> [profile_name] [hue_divs] [sat_divs] [val_divs]\n"
                << "Defaults: profile_name=\"Converted Look\", hue_divs=90, sat_divs=25, val_divs=16\n";
            return 1;
        }

        const std::string cubePath = argv[1];
        const std::string xmpPath = argv[2];
        const std::string profileName = (argc >= 4) ? argv[3] : "Converted Look";
        const uint32_t hueDivs = (argc >= 5) ? static_cast<uint32_t>(std::stoul(argv[4])) : 90;
        const uint32_t satDivs = (argc >= 6) ? static_cast<uint32_t>(std::stoul(argv[5])) : 25;
        const uint32_t valDivs = (argc >= 7) ? static_cast<uint32_t>(std::stoul(argv[6])) : 16;

        if (hueDivs < 1 || satDivs < 2 || valDivs < 1) {
            throw std::runtime_error("Invalid map dimensions: need hue>=1, sat>=2, val>=1");
        }

        const CubeLut3D cube = ParseCube3D(cubePath);

        dng_hue_sat_map map = BuildHueSatMapFromCube(cube, hueDivs, satDivs, valDivs);

        dng_look_table lookTable;
        lookTable.Set(map, encoding_Linear);

        dng_host host;
        AutoPtr<dng_memory_block> encodedBlock(lookTable.EncodeAsString(host.Allocator()));
        const std::string encodedTable(encodedBlock->Buffer_char(),
                                       static_cast<size_t>(encodedBlock->LogicalSize()));

        const std::string fpHex = FingerprintToHex(lookTable.Fingerprint());

        WriteLookXmp(xmpPath, profileName, lookTable, encodedTable, host.Allocator());

        std::cout << "Wrote XMP: " << xmpPath << "\n";
        std::cout << "LookTable fingerprint: " << fpHex << "\n";
        std::cout << "Encoded table size: " << encodedTable.size() << " bytes\n";

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 2;
    } catch (...) {
        std::cerr << "Error: unknown failure\n";
        return 3;
    }
}
