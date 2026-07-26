#include "ModMain.h"

#include "CoopPortraitAssets.generated.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "../ThirdParty/stb/stb_image.h"

namespace
{
struct EmbeddedPortrait
{
    const unsigned char* bytes;
    unsigned int byteCount;
    const char* textureName;
};

const std::array<EmbeddedPortrait, 6> kEmbeddedPortraits = {{
    {coop_portrait_danielle_jpg, coop_portrait_danielle_jpg_len, "Coop Portrait Danielle Sho"},
    {coop_portrait_morgan_male_jpg, coop_portrait_morgan_male_jpg_len, "Coop Portrait Morgan Male"},
    {coop_portrait_morgan_female_jpg, coop_portrait_morgan_female_jpg_len, "Coop Portrait Morgan Female"},
    {coop_portrait_bellamy_jpg, coop_portrait_bellamy_jpg_len, "Coop Portrait Sylvain Bellamy"},
    {coop_portrait_grant_lockwood_jpg, coop_portrait_grant_lockwood_jpg_len, "Coop Portrait Grant Lockwood"},
    {coop_portrait_mariana_arias_jpg, coop_portrait_mariana_arias_jpg_len, "Coop Portrait Mariana Arias"},
}};
}

void ModMain::EnsurePlayerPortraitTextures()
{
    if (m_playerPortraitTexturesLoaded || !gEnv || !gEnv->pRenderer)
        return;

    m_playerPortraitTexturesLoaded = true;
    for (size_t index = 0; index < kEmbeddedPortraits.size(); ++index)
    {
        const EmbeddedPortrait& source = kEmbeddedPortraits[index];
        int width = 0;
        int height = 0;
        int channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            source.bytes,
            static_cast<int>(source.byteCount),
            &width,
            &height,
            &channels,
            4);
        if (!pixels || width <= 0 || height <= 0)
        {
            stbi_image_free(pixels);
            continue;
        }

        m_playerPortraitTextures[index].Assign_NoAddRef(gEnv->pRenderer->CreateTexture(
            source.textureName,
            width,
            height,
            1,
            pixels,
            eTF_R8G8B8A8,
            FT_TEX_FONT));
        stbi_image_free(pixels);
    }
}

void ModMain::ReleasePlayerPortraitTextures()
{
    for (auto& texture : m_playerPortraitTextures)
        texture = nullptr;
    m_playerPortraitTexturesLoaded = false;
}

ITexture* ModMain::GetPlayerPortraitTexture(size_t index) const
{
    return index < m_playerPortraitTextures.size() ? m_playerPortraitTextures[index].get() : nullptr;
}
