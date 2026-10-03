#include "lighting.h"

// The lighting shader is kept in source to avoid an external runtime asset.
// Same sun and ambient as GrassColor() in terrain.cpp, so models match the hills.

static const char *VS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matNormal;
out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

static const char *FS = R"(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
out vec4 finalColor;
void main()
{
    vec4 base = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
    float diff = max(dot(normalize(fragNormal), normalize(vec3(0.5, 0.7, 0.35))), 0.0);
    float lit = 0.55 + 0.60 * diff;
    finalColor = vec4(min(base.rgb * lit, vec3(1.0)), base.a);
}
)";

Shader Lighting_LoadShader(void)
{
    return LoadShaderFromMemory(VS, FS);
}

void Lighting_Attach(Model *m, Shader shader)
{
    for (int i = 0; i < m->materialCount; i++) m->materials[i].shader = shader;
}

void Lighting_Detach(Model *m)
{
    // Put raylib's default shader back so UnloadModel can never free our shader.
    Material def = LoadMaterialDefault();
    for (int i = 0; i < m->materialCount; i++) m->materials[i].shader = def.shader;
    MemFree(def.maps);
}
