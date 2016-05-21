
/*
    pbrt source code is Copyright(c) 1998-2016
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */


// materials/fourier.cpp*
#include "materials/fourier.h"
#include "paramset.h"
#include "interaction.h"

// FourierMaterial Method Definitions
/*
  File format description:

  This is the file format generated by the material designer of the paper

  'A Comprehensive Framework for Rendering Layered Materials' by
  Wenzel Jakob, Eugene D'Eon, Otto Jakob and Steve Marschner
  Transactions on Graphics (Proceedings of SIGGRAPH 2014)

  A standalone Python plugin for generating such data files is available
  on GitHub: https://github.com/wjakob/layerlab

  This format specifies an isotropic BSDF expressed in a Spline x Fourier
  directional basis. It begins with a header of the following type:

 struct Header {
     uint8_t identifier[7];     // Set to 'SCATFUN'
     uint8_t version;           // Currently version is 1
     uint32_t flags;            // 0x01: file contains a BSDF, 0x02: uses
 harmonic extrapolation
     int nMu;                   // Number of samples in the elevational
 discretization

     int nCoeffs;               // Total number of Fourier series coefficients
 stored in the file
     int mMax;                  // Coeff. count for the longest series occurring
 in the file
     int nChannels;             // Number of color channels (usually 1 or 3)
     int nBases;                // Number of BSDF basis functions (relevant for
 texturing)

     int nMetadataBytes;        // Size of descriptive metadata that follows the
 BSDF data
     int nParameters;           // Number of textured material parameters
     int nParameterValues;      // Total number of BSDF samples for all textured
 parameters
     float eta;                 // Relative IOR through the material
 (eta(bottom) / eta(top))

     float alpha[2];            // Beckmann-equiv. roughness on the top (0) and
 bottom (1) side
     float unused[2];           // Unused fields to pad the header to 64 bytes
 };

  Due to space constraints, two features are not currently implemented in PBRT,
  namely texturing and harmonic extrapolation (though it would be
 straightforward
  to port them from Mitsuba.)
*/

inline bool IsBigEndian() {
    uint32_t i = 0x01020304;
    char c[4];
    memcpy(c, &i, 4);
    return (c[0] == 1);
}

bool FourierBSDFTable::Read(const std::string &filename,
                            FourierBSDFTable *bsdfTable) {
    bsdfTable->mu = bsdfTable->cdf = bsdfTable->a = nullptr;
    bsdfTable->aOffset = bsdfTable->m = nullptr;

    FILE *f = fopen(filename.c_str(), "rb");

    if (!f) {
        Error("Unable to open tabulated BSDF file \"%s\"", filename.c_str());
        return false;
    }

    auto read32 = [&](void *target, size_t count) -> bool {
        if (fread(target, sizeof(int), count, f) != count) return false;
        if (IsBigEndian()) {
            int32_t *tmp = (int32_t *)target;
            for (size_t i = 0; i < count; ++i) {
#ifndef PBRT_IS_MSVC
                tmp[i] = __builtin_bswap32(tmp[i]);
#else
                tmp[i] = _byteswap_ulong(tmp[i]);
#endif
            }
        }
        return true;
    };
    auto readfloat = [&](Float *target, size_t count) -> bool {
        if (sizeof(*target) == sizeof(float)) return read32(target, count);

        std::unique_ptr<float[]> buf(new float[count]);
        bool ret = read32(buf.get(), count);
        for (size_t i = 0; i < count; ++i) target[i] = buf[i];
        return ret;
    };

    const char header_exp[8] = {'S', 'C', 'A', 'T', 'F', 'U', 'N', '\x01'};
    char header[8];
    std::unique_ptr<int[]> offsetAndLength;

    if (fread(header, 1, 8, f) != 8 || memcmp(header, header_exp, 8) != 0)
        goto fail;

    int flags, nCoeffs, nBases, unused[4];

    if (!read32(&flags, 1) || !read32(&bsdfTable->nMu, 1) ||
        !read32(&nCoeffs, 1) || !read32(&bsdfTable->mMax, 1) ||
        !read32(&bsdfTable->nChannels, 1) || !read32(&nBases, 1) ||
        !read32(unused, 3) || !readfloat(&bsdfTable->eta, 1) ||
        !read32(&unused, 4))
        goto fail;

    /* Only a subset of BSDF files are supported for simplicity, in particular:
       monochromatic and
       RGB files with uniform (i.e. non-textured) material properties */
    if (flags != 1 ||
        (bsdfTable->nChannels != 1 && bsdfTable->nChannels != 3) || nBases != 1)
        goto fail;

    bsdfTable->mu = new Float[bsdfTable->nMu];
    bsdfTable->cdf = new Float[bsdfTable->nMu * bsdfTable->nMu];
    bsdfTable->a0 = new Float[bsdfTable->nMu * bsdfTable->nMu];
    offsetAndLength.reset(new int[bsdfTable->nMu * bsdfTable->nMu * 2]);
    bsdfTable->aOffset = new int[bsdfTable->nMu * bsdfTable->nMu];
    bsdfTable->m = new int[bsdfTable->nMu * bsdfTable->nMu];
    bsdfTable->a = new Float[nCoeffs];

    if (!readfloat(bsdfTable->mu, bsdfTable->nMu) ||
        !readfloat(bsdfTable->cdf, bsdfTable->nMu * bsdfTable->nMu) ||
        !read32(offsetAndLength.get(), bsdfTable->nMu * bsdfTable->nMu * 2) ||
        !readfloat(bsdfTable->a, nCoeffs))
        goto fail;

    for (int i = 0; i < bsdfTable->nMu * bsdfTable->nMu; ++i) {
        int offset = offsetAndLength[2 * i],
            length = offsetAndLength[2 * i + 1];

        bsdfTable->aOffset[i] = offset;
        bsdfTable->m[i] = length;

        bsdfTable->a0[i] = length > 0 ? bsdfTable->a[offset] : (Float)0;
    }

    bsdfTable->recip = new Float[bsdfTable->mMax];
    for (int i = 0; i < bsdfTable->mMax; ++i)
        bsdfTable->recip[i] = 1 / (Float)i;

    fclose(f);
    return true;
fail:
    fclose(f);
    Error(
        "Tabulated BSDF file \"%s\" has an incompatible file format or "
        "version.",
        filename.c_str());
    return false;
}

FourierMaterial::FourierMaterial(const std::string &filename,
                                 const std::shared_ptr<Texture<Float>> &bumpMap)
    : bumpMap(bumpMap) {
    FourierBSDFTable::Read(filename, &bsdfTable);
}

void FourierMaterial::ComputeScatteringFunctions(
    SurfaceInteraction *si, MemoryArena &arena, TransportMode mode,
    bool allowMultipleLobes) const {
    // Perform bump mapping with _bumpMap_, if present
    if (bumpMap) Bump(bumpMap, si);
    si->bsdf = ARENA_ALLOC(arena, BSDF)(*si);
    // Checking for zero channels works as a proxy for checking whether the
    // table was successfully read from the file.
    if (bsdfTable.nChannels > 0)
        si->bsdf->Add(ARENA_ALLOC(arena, FourierBSDF)(bsdfTable, mode));
}

FourierMaterial *CreateFourierMaterial(const TextureParams &mp) {
    std::shared_ptr<Texture<Float>> bumpMap =
        mp.GetFloatTextureOrNull("bumpmap");
    return new FourierMaterial(mp.FindFilename("bsdffile"), bumpMap);
}
