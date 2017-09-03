// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/UberShader.h"

using namespace Eigen;
using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

UberShader::UberShader()
: mColorMap{GL_CLAMP_TO_EDGE, GL_LINEAR, false} {
    mShader.define("SRGB",        to_string(ETonemap::SRGB));
    mShader.define("GAMMA",       to_string(ETonemap::Gamma));
    mShader.define("FALSE_COLOR", to_string(ETonemap::FalseColor));
    mShader.define("POS_NEG",     to_string(ETonemap::PositiveNegative));

    mShader.define("ERROR",                   to_string(EMetric::Error));
    mShader.define("ABSOLUTE_ERROR",          to_string(EMetric::AbsoluteError));
    mShader.define("SQUARED_ERROR",           to_string(EMetric::SquaredError));
    mShader.define("RELATIVE_ABSOLUTE_ERROR", to_string(EMetric::RelativeAbsoluteError));
    mShader.define("RELATIVE_SQUARED_ERROR",  to_string(EMetric::RelativeSquaredError));

    mShader.init(
        "ubershader",

        // Vertex shader
        R"(#version 330
        uniform mat3 imageTransform;
        uniform mat3 referenceTransform;

        in vec2 position;

        out vec2 imageUv;
        out vec2 referenceUv;

        void main() {
            imageUv = (imageTransform * vec3(position, 1.0)).xy;
            referenceUv = (referenceTransform * vec3(position, 1.0)).xy;

            gl_Position = vec4(position, 1.0, 1.0);
        })",

        // Fragment shader
        R"(#version 330

        uniform sampler2D imageRed;
        uniform sampler2D imageGreen;
        uniform sampler2D imageBlue;
        uniform sampler2D imageAlpha;

        uniform sampler2D referenceRed;
        uniform sampler2D referenceGreen;
        uniform sampler2D referenceBlue;
        uniform sampler2D referenceAlpha;
        uniform bool hasReference;

        uniform sampler2D colormap;

        uniform float exposure;
        uniform float offset;
        uniform int tonemap;
        uniform int metric;

        in vec2 imageUv;
        in vec2 referenceUv;

        out vec4 color;

        float average(vec3 col) {
            return (col.r + col.g + col.b) / 3.0;
        }

        vec3 applyExposureAndOffset(vec3 col) {
            return pow(2.0, exposure) * col + offset;
        }

        vec3 falseColor(float v) {
            v = clamp(v, 0.0, 1.0);
            return texture(colormap, vec2(v, 0.5)).rgb;
        }

        float sRGB(float linear) {
            if (linear > 1.0) {
                return 1.0;
            } else if (linear < 0.0) {
                return 0.0;
            } else if (linear < 0.0031308) {
                return 12.92 * linear;
            } else {
                return 1.055 * pow(linear, 0.41666) - 0.055;
            }
        }

        vec3 applyTonemap(vec3 col) {
            switch (tonemap) {
                case SRGB:        return vec3(sRGB(col.r), sRGB(col.g), sRGB(col.b));
                case GAMMA:       return pow(col, vec3(1.0 / 2.2));
                // Here grayscale is compressed such that the darkest color is is 1/1024th as bright as the brightest color.
                case FALSE_COLOR: return falseColor(log2(average(col)) / 10.0 + 0.5);
                case POS_NEG:     return vec3(-average(min(col, vec3(0.0))) / 0.5, average(max(col, vec3(0.0))) / 0.5, 0.0);
            }
            return vec3(0.0);
        }

        vec3 applyMetric(vec3 col, vec3 reference) {
            switch (metric) {
                case ERROR:                   return col;
                case ABSOLUTE_ERROR:          return abs(col);
                case SQUARED_ERROR:           return col * col;
                case RELATIVE_ABSOLUTE_ERROR: return abs(col) / (reference + vec3(0.01));
                case RELATIVE_SQUARED_ERROR:  return col * col / (reference * reference + vec3(0.0001));
            }
            return vec3(0.0);
        }

        float sample(sampler2D sampler, vec2 uv) {
            if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
                return 0.0;
            }
            return texture(sampler, imageUv).x;
        }

        void main() {
            vec4 image = vec4(
                sample(imageRed, imageUv),
                sample(imageGreen, imageUv),
                sample(imageBlue, imageUv),
                sample(imageAlpha, imageUv)
            );

            if (!hasReference) {
                color = vec4(applyTonemap(applyExposureAndOffset(image.rgb)), image.a);
                return;
            }

            vec4 reference = vec4(
                sample(referenceRed, referenceUv),
                sample(referenceGreen, referenceUv),
                sample(referenceBlue, referenceUv),
                sample(referenceAlpha, referenceUv)
            );

            vec3 difference = image.rgb - reference.rgb;
            float alpha = (image.a + reference.a) * 0.5;
            color = vec4(applyTonemap(applyExposureAndOffset(applyMetric(difference, reference.rgb))), alpha);
        })"
    );

    // 2 Triangles
    MatrixXu indices(3, 2);
    indices.col(0) << 0, 1, 2;
    indices.col(1) << 2, 3, 0;

    MatrixXf positions(2, 4);
    positions.col(0) << -1, -1;
    positions.col(1) <<  1, -1;
    positions.col(2) <<  1,  1;
    positions.col(3) << -1,  1;

    mShader.bind();
    mShader.uploadIndices(indices);
    mShader.uploadAttrib("position", positions);

    vector<float> falseColorData = {
        0.267004f, 0.004874f, 0.329415f, 1.0f,
        0.26851f, 0.009605f, 0.335427f, 1.0f,
        0.269944f, 0.014625f, 0.341379f, 1.0f,
        0.271305f, 0.019942f, 0.347269f, 1.0f,
        0.272594f, 0.025563f, 0.353093f, 1.0f,
        0.273809f, 0.031497f, 0.358853f, 1.0f,
        0.274952f, 0.037752f, 0.364543f, 1.0f,
        0.276022f, 0.044167f, 0.370164f, 1.0f,
        0.277018f, 0.050344f, 0.375715f, 1.0f,
        0.277941f, 0.056324f, 0.381191f, 1.0f,
        0.278791f, 0.062145f, 0.386592f, 1.0f,
        0.279566f, 0.067836f, 0.391917f, 1.0f,
        0.280267f, 0.073417f, 0.397163f, 1.0f,
        0.280894f, 0.078907f, 0.402329f, 1.0f,
        0.281446f, 0.08432f, 0.407414f, 1.0f,
        0.281924f, 0.089666f, 0.412415f, 1.0f,
        0.282327f, 0.094955f, 0.417331f, 1.0f,
        0.282656f, 0.100196f, 0.42216f, 1.0f,
        0.28291f, 0.105393f, 0.426902f, 1.0f,
        0.283091f, 0.110553f, 0.431554f, 1.0f,
        0.283197f, 0.11568f, 0.436115f, 1.0f,
        0.283229f, 0.120777f, 0.440584f, 1.0f,
        0.283187f, 0.125848f, 0.44496f, 1.0f,
        0.283072f, 0.130895f, 0.449241f, 1.0f,
        0.282884f, 0.13592f, 0.453427f, 1.0f,
        0.282623f, 0.140926f, 0.457517f, 1.0f,
        0.28229f, 0.145912f, 0.46151f, 1.0f,
        0.281887f, 0.150881f, 0.465405f, 1.0f,
        0.281412f, 0.155834f, 0.469201f, 1.0f,
        0.280868f, 0.160771f, 0.472899f, 1.0f,
        0.280255f, 0.165693f, 0.476498f, 1.0f,
        0.279574f, 0.170599f, 0.479997f, 1.0f,
        0.278826f, 0.17549f, 0.483397f, 1.0f,
        0.278012f, 0.180367f, 0.486697f, 1.0f,
        0.277134f, 0.185228f, 0.489898f, 1.0f,
        0.276194f, 0.190074f, 0.493001f, 1.0f,
        0.275191f, 0.194905f, 0.496005f, 1.0f,
        0.274128f, 0.199721f, 0.498911f, 1.0f,
        0.273006f, 0.20452f, 0.501721f, 1.0f,
        0.271828f, 0.209303f, 0.504434f, 1.0f,
        0.270595f, 0.214069f, 0.507052f, 1.0f,
        0.269308f, 0.218818f, 0.509577f, 1.0f,
        0.267968f, 0.223549f, 0.512008f, 1.0f,
        0.26658f, 0.228262f, 0.514349f, 1.0f,
        0.265145f, 0.232956f, 0.516599f, 1.0f,
        0.263663f, 0.237631f, 0.518762f, 1.0f,
        0.262138f, 0.242286f, 0.520837f, 1.0f,
        0.260571f, 0.246922f, 0.522828f, 1.0f,
        0.258965f, 0.251537f, 0.524736f, 1.0f,
        0.257322f, 0.25613f, 0.526563f, 1.0f,
        0.255645f, 0.260703f, 0.528312f, 1.0f,
        0.253935f, 0.265254f, 0.529983f, 1.0f,
        0.252194f, 0.269783f, 0.531579f, 1.0f,
        0.250425f, 0.27429f, 0.533103f, 1.0f,
        0.248629f, 0.278775f, 0.534556f, 1.0f,
        0.246811f, 0.283237f, 0.535941f, 1.0f,
        0.244972f, 0.287675f, 0.53726f, 1.0f,
        0.243113f, 0.292092f, 0.538516f, 1.0f,
        0.241237f, 0.296485f, 0.539709f, 1.0f,
        0.239346f, 0.300855f, 0.540844f, 1.0f,
        0.237441f, 0.305202f, 0.541921f, 1.0f,
        0.235526f, 0.309527f, 0.542944f, 1.0f,
        0.233603f, 0.313828f, 0.543914f, 1.0f,
        0.231674f, 0.318106f, 0.544834f, 1.0f,
        0.229739f, 0.322361f, 0.545706f, 1.0f,
        0.227802f, 0.326594f, 0.546532f, 1.0f,
        0.225863f, 0.330805f, 0.547314f, 1.0f,
        0.223925f, 0.334994f, 0.548053f, 1.0f,
        0.221989f, 0.339161f, 0.548752f, 1.0f,
        0.220057f, 0.343307f, 0.549413f, 1.0f,
        0.21813f, 0.347432f, 0.550038f, 1.0f,
        0.21621f, 0.351535f, 0.550627f, 1.0f,
        0.214298f, 0.355619f, 0.551184f, 1.0f,
        0.212395f, 0.359683f, 0.55171f, 1.0f,
        0.210503f, 0.363727f, 0.552206f, 1.0f,
        0.208623f, 0.367752f, 0.552675f, 1.0f,
        0.206756f, 0.371758f, 0.553117f, 1.0f,
        0.204903f, 0.375746f, 0.553533f, 1.0f,
        0.203063f, 0.379716f, 0.553925f, 1.0f,
        0.201239f, 0.38367f, 0.554294f, 1.0f,
        0.19943f, 0.387607f, 0.554642f, 1.0f,
        0.197636f, 0.391528f, 0.554969f, 1.0f,
        0.19586f, 0.395433f, 0.555276f, 1.0f,
        0.1941f, 0.399323f, 0.555565f, 1.0f,
        0.192357f, 0.403199f, 0.555836f, 1.0f,
        0.190631f, 0.407061f, 0.556089f, 1.0f,
        0.188923f, 0.41091f, 0.556326f, 1.0f,
        0.187231f, 0.414746f, 0.556547f, 1.0f,
        0.185556f, 0.41857f, 0.556753f, 1.0f,
        0.183898f, 0.422383f, 0.556944f, 1.0f,
        0.182256f, 0.426184f, 0.55712f, 1.0f,
        0.180629f, 0.429975f, 0.557282f, 1.0f,
        0.179019f, 0.433756f, 0.55743f, 1.0f,
        0.177423f, 0.437527f, 0.557565f, 1.0f,
        0.175841f, 0.44129f, 0.557685f, 1.0f,
        0.174274f, 0.445044f, 0.557792f, 1.0f,
        0.172719f, 0.448791f, 0.557885f, 1.0f,
        0.171176f, 0.45253f, 0.557965f, 1.0f,
        0.169646f, 0.456262f, 0.55803f, 1.0f,
        0.168126f, 0.459988f, 0.558082f, 1.0f,
        0.166617f, 0.463708f, 0.558119f, 1.0f,
        0.165117f, 0.467423f, 0.558141f, 1.0f,
        0.163625f, 0.471133f, 0.558148f, 1.0f,
        0.162142f, 0.474838f, 0.55814f, 1.0f,
        0.160665f, 0.47854f, 0.558115f, 1.0f,
        0.159194f, 0.482237f, 0.558073f, 1.0f,
        0.157729f, 0.485932f, 0.558013f, 1.0f,
        0.15627f, 0.489624f, 0.557936f, 1.0f,
        0.154815f, 0.493313f, 0.55784f, 1.0f,
        0.153364f, 0.497f, 0.557724f, 1.0f,
        0.151918f, 0.500685f, 0.557587f, 1.0f,
        0.150476f, 0.504369f, 0.55743f, 1.0f,
        0.149039f, 0.508051f, 0.55725f, 1.0f,
        0.147607f, 0.511733f, 0.557049f, 1.0f,
        0.14618f, 0.515413f, 0.556823f, 1.0f,
        0.144759f, 0.519093f, 0.556572f, 1.0f,
        0.143343f, 0.522773f, 0.556295f, 1.0f,
        0.141935f, 0.526453f, 0.555991f, 1.0f,
        0.140536f, 0.530132f, 0.555659f, 1.0f,
        0.139147f, 0.533812f, 0.555298f, 1.0f,
        0.13777f, 0.537492f, 0.554906f, 1.0f,
        0.136408f, 0.541173f, 0.554483f, 1.0f,
        0.135066f, 0.544853f, 0.554029f, 1.0f,
        0.133743f, 0.548535f, 0.553541f, 1.0f,
        0.132444f, 0.552216f, 0.553018f, 1.0f,
        0.131172f, 0.555899f, 0.552459f, 1.0f,
        0.129933f, 0.559582f, 0.551864f, 1.0f,
        0.128729f, 0.563265f, 0.551229f, 1.0f,
        0.127568f, 0.566949f, 0.550556f, 1.0f,
        0.126453f, 0.570633f, 0.549841f, 1.0f,
        0.125394f, 0.574318f, 0.549086f, 1.0f,
        0.124395f, 0.578002f, 0.548287f, 1.0f,
        0.123463f, 0.581687f, 0.547445f, 1.0f,
        0.122606f, 0.585371f, 0.546557f, 1.0f,
        0.121831f, 0.589055f, 0.545623f, 1.0f,
        0.121148f, 0.592739f, 0.544641f, 1.0f,
        0.120565f, 0.596422f, 0.543611f, 1.0f,
        0.120092f, 0.600104f, 0.54253f, 1.0f,
        0.119738f, 0.603785f, 0.5414f, 1.0f,
        0.119512f, 0.607464f, 0.540218f, 1.0f,
        0.119423f, 0.611141f, 0.538982f, 1.0f,
        0.119483f, 0.614817f, 0.537692f, 1.0f,
        0.119699f, 0.61849f, 0.536347f, 1.0f,
        0.120081f, 0.622161f, 0.534946f, 1.0f,
        0.120638f, 0.625828f, 0.533488f, 1.0f,
        0.12138f, 0.629492f, 0.531973f, 1.0f,
        0.122312f, 0.633153f, 0.530398f, 1.0f,
        0.123444f, 0.636809f, 0.528763f, 1.0f,
        0.12478f, 0.640461f, 0.527068f, 1.0f,
        0.126326f, 0.644107f, 0.525311f, 1.0f,
        0.128087f, 0.647749f, 0.523491f, 1.0f,
        0.130067f, 0.651384f, 0.521608f, 1.0f,
        0.132268f, 0.655014f, 0.519661f, 1.0f,
        0.134692f, 0.658636f, 0.517649f, 1.0f,
        0.137339f, 0.662252f, 0.515571f, 1.0f,
        0.14021f, 0.665859f, 0.513427f, 1.0f,
        0.143303f, 0.669459f, 0.511215f, 1.0f,
        0.146616f, 0.67305f, 0.508936f, 1.0f,
        0.150148f, 0.676631f, 0.506589f, 1.0f,
        0.153894f, 0.680203f, 0.504172f, 1.0f,
        0.157851f, 0.683765f, 0.501686f, 1.0f,
        0.162016f, 0.687316f, 0.499129f, 1.0f,
        0.166383f, 0.690856f, 0.496502f, 1.0f,
        0.170948f, 0.694384f, 0.493803f, 1.0f,
        0.175707f, 0.6979f, 0.491033f, 1.0f,
        0.180653f, 0.701402f, 0.488189f, 1.0f,
        0.185783f, 0.704891f, 0.485273f, 1.0f,
        0.19109f, 0.708366f, 0.482284f, 1.0f,
        0.196571f, 0.711827f, 0.479221f, 1.0f,
        0.202219f, 0.715272f, 0.476084f, 1.0f,
        0.20803f, 0.718701f, 0.472873f, 1.0f,
        0.214f, 0.722114f, 0.469588f, 1.0f,
        0.220124f, 0.725509f, 0.466226f, 1.0f,
        0.226397f, 0.728888f, 0.462789f, 1.0f,
        0.232815f, 0.732247f, 0.459277f, 1.0f,
        0.239374f, 0.735588f, 0.455688f, 1.0f,
        0.24607f, 0.73891f, 0.452024f, 1.0f,
        0.252899f, 0.742211f, 0.448284f, 1.0f,
        0.259857f, 0.745492f, 0.444467f, 1.0f,
        0.266941f, 0.748751f, 0.440573f, 1.0f,
        0.274149f, 0.751988f, 0.436601f, 1.0f,
        0.281477f, 0.755203f, 0.432552f, 1.0f,
        0.288921f, 0.758394f, 0.428426f, 1.0f,
        0.296479f, 0.761561f, 0.424223f, 1.0f,
        0.304148f, 0.764704f, 0.419943f, 1.0f,
        0.311925f, 0.767822f, 0.415586f, 1.0f,
        0.319809f, 0.770914f, 0.411152f, 1.0f,
        0.327796f, 0.77398f, 0.40664f, 1.0f,
        0.335885f, 0.777018f, 0.402049f, 1.0f,
        0.344074f, 0.780029f, 0.397381f, 1.0f,
        0.35236f, 0.783011f, 0.392636f, 1.0f,
        0.360741f, 0.785964f, 0.387814f, 1.0f,
        0.369214f, 0.788888f, 0.382914f, 1.0f,
        0.377779f, 0.791781f, 0.377939f, 1.0f,
        0.386433f, 0.794644f, 0.372886f, 1.0f,
        0.395174f, 0.797475f, 0.367757f, 1.0f,
        0.404001f, 0.800275f, 0.362552f, 1.0f,
        0.412913f, 0.803041f, 0.357269f, 1.0f,
        0.421908f, 0.805774f, 0.35191f, 1.0f,
        0.430983f, 0.808473f, 0.346476f, 1.0f,
        0.440137f, 0.811138f, 0.340967f, 1.0f,
        0.449368f, 0.813768f, 0.335384f, 1.0f,
        0.458674f, 0.816363f, 0.329727f, 1.0f,
        0.468053f, 0.818921f, 0.323998f, 1.0f,
        0.477504f, 0.821444f, 0.318195f, 1.0f,
        0.487026f, 0.823929f, 0.312321f, 1.0f,
        0.496615f, 0.826376f, 0.306377f, 1.0f,
        0.506271f, 0.828786f, 0.300362f, 1.0f,
        0.515992f, 0.831158f, 0.294279f, 1.0f,
        0.525776f, 0.833491f, 0.288127f, 1.0f,
        0.535621f, 0.835785f, 0.281908f, 1.0f,
        0.545524f, 0.838039f, 0.275626f, 1.0f,
        0.555484f, 0.840254f, 0.269281f, 1.0f,
        0.565498f, 0.84243f, 0.262877f, 1.0f,
        0.575563f, 0.844566f, 0.256415f, 1.0f,
        0.585678f, 0.846661f, 0.249897f, 1.0f,
        0.595839f, 0.848717f, 0.243329f, 1.0f,
        0.606045f, 0.850733f, 0.236712f, 1.0f,
        0.616293f, 0.852709f, 0.230052f, 1.0f,
        0.626579f, 0.854645f, 0.223353f, 1.0f,
        0.636902f, 0.856542f, 0.21662f, 1.0f,
        0.647257f, 0.8584f, 0.209861f, 1.0f,
        0.657642f, 0.860219f, 0.203082f, 1.0f,
        0.668054f, 0.861999f, 0.196293f, 1.0f,
        0.678489f, 0.863742f, 0.189503f, 1.0f,
        0.688944f, 0.865448f, 0.182725f, 1.0f,
        0.699415f, 0.867117f, 0.175971f, 1.0f,
        0.709898f, 0.868751f, 0.169257f, 1.0f,
        0.720391f, 0.87035f, 0.162603f, 1.0f,
        0.730889f, 0.871916f, 0.156029f, 1.0f,
        0.741388f, 0.873449f, 0.149561f, 1.0f,
        0.751884f, 0.874951f, 0.143228f, 1.0f,
        0.762373f, 0.876424f, 0.137064f, 1.0f,
        0.772852f, 0.877868f, 0.131109f, 1.0f,
        0.783315f, 0.879285f, 0.125405f, 1.0f,
        0.79376f, 0.880678f, 0.120005f, 1.0f,
        0.804182f, 0.882046f, 0.114965f, 1.0f,
        0.814576f, 0.883393f, 0.110347f, 1.0f,
        0.82494f, 0.88472f, 0.106217f, 1.0f,
        0.83527f, 0.886029f, 0.102646f, 1.0f,
        0.845561f, 0.887322f, 0.099702f, 1.0f,
        0.85581f, 0.888601f, 0.097452f, 1.0f,
        0.866013f, 0.889868f, 0.095953f, 1.0f,
        0.876168f, 0.891125f, 0.09525f, 1.0f,
        0.886271f, 0.892374f, 0.095374f, 1.0f,
        0.89632f, 0.893616f, 0.096335f, 1.0f,
        0.906311f, 0.894855f, 0.098125f, 1.0f,
        0.916242f, 0.896091f, 0.100717f, 1.0f,
        0.926106f, 0.89733f, 0.104071f, 1.0f,
        0.935904f, 0.89857f, 0.108131f, 1.0f,
        0.945636f, 0.899815f, 0.112838f, 1.0f,
        0.9553f, 0.901065f, 0.118128f, 1.0f,
        0.964894f, 0.902323f, 0.123941f, 1.0f,
        0.974417f, 0.90359f, 0.130215f, 1.0f,
        0.983868f, 0.904867f, 0.136897f, 1.0f,
        0.993248f, 0.906157f, 0.143936f, 1.0f,
    };

    mColorMap.setData(falseColorData, Vector2i{(int)falseColorData.size() / 4, 1}, 4);
}

UberShader::~UberShader() {
    mShader.free();
}

void UberShader::draw(
    std::array<const GlTexture*, 4> texturesImage,
    const Eigen::Matrix3f& transformImage,
    std::array<const GlTexture*, 4> texturesReference,
    const Eigen::Matrix3f& transformReference,
    float exposure,
    float offset,
    ETonemap tonemap,
    EMetric metric
) {
    bindImageData(texturesImage, transformImage, exposure, offset, tonemap);
    bindReferenceData(texturesReference, transformReference, metric);
    mShader.setUniform("hasReference", true);
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

void UberShader::draw(
    std::array<const GlTexture*, 4> texturesImage,
    const Eigen::Matrix3f& transformImage,
    float exposure,
    float offset,
    ETonemap tonemap
) {
    bindImageData(texturesImage, transformImage, exposure, offset, tonemap);
    mShader.setUniform("hasReference", false);
    mShader.drawIndexed(GL_TRIANGLES, 0, 2);
}

void UberShader::bindReferenceData(
    std::array<const GlTexture*, 4> texturesReference,
    const Eigen::Matrix3f& transformReference,
    EMetric metric
) {
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + 4 + i);
        glBindTexture(GL_TEXTURE_2D, texturesReference[i]->id());
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mShader.setUniform("referenceRed",   4);
    mShader.setUniform("referenceGreen", 5);
    mShader.setUniform("referenceBlue",  6);
    mShader.setUniform("referenceAlpha", 7);
    mShader.setUniform("referenceTransform", transformReference);

    mShader.setUniform("metric", static_cast<int>(metric));
}

void UberShader::bindImageData(
    std::array<const GlTexture*, 4> texturesImage,
    const Eigen::Matrix3f& transformImage,
    float exposure,
    float offset,
    ETonemap tonemap
) {
    for (int i = 0; i < 4; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, texturesImage[i]->id());
    }

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    mShader.bind();
    mShader.setUniform("imageRed", 0);
    mShader.setUniform("imageGreen", 1);
    mShader.setUniform("imageBlue", 2);
    mShader.setUniform("imageAlpha", 3);
    mShader.setUniform("imageTransform", transformImage);

    mShader.setUniform("exposure", exposure);
    mShader.setUniform("offset", offset);
    mShader.setUniform("tonemap", static_cast<int>(tonemap));

    glActiveTexture(GL_TEXTURE10);
    glBindTexture(GL_TEXTURE_2D, mColorMap.id());
    mShader.setUniform("colormap", 10);
}

TEV_NAMESPACE_END
