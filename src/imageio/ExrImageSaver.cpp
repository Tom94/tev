// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/imageio/ExrImageSaver.h>

#include <ImfChannelList.h>
#include <ImfOutputFile.h>
#include <ImfInputPart.h>
#include <ImfMultiPartInputFile.h>
#include <Iex.h>

#include <ostream>
#include <string>
#include <vector>

using namespace nanogui;
using namespace std;

TEV_NAMESPACE_BEGIN

class StdOStream: public Imf::OStream
{
public:
    StdOStream(ostream& stream, const char fileName[])
    : Imf::OStream{fileName}, mStream{stream} { }

    void write(const char c[/*n*/], int n) {
        clearError();
        mStream.write (c, n);
        checkError(mStream);
    }

    Imf::Int64 tellp() {
        return std::streamoff(mStream.tellp());
    }

    void seekp(Imf::Int64 pos) {
        mStream.seekp(pos);
        checkError(mStream);
    }

private:
    // The following error-checking functions were copy&pasted from the OpenEXR source code
    static void clearError() {
        errno = 0;
    }

    static void checkError(ostream& os) {
        if (!os) {
            if (errno) {
                IEX_NAMESPACE::throwErrnoExc();
            }

            throw IEX_NAMESPACE::ErrnoExc("File output failed.");
        }
    }

    ostream& mStream;
};

void ExrImageSaver::save(ostream& oStream, const fs::path& path, const vector<float>& data, const Vector2i& imageSize, int nChannels) const {
    vector<string> channelNames = {
        "R", "G", "B", "A",
    };

    if (nChannels <= 0 || nChannels > 4) {
        throw invalid_argument{format("Invalid number of channels {}.", nChannels)};
    }

    Imf::Header header{imageSize.x(), imageSize.y()};
    Imf::FrameBuffer frameBuffer;

    for (int i = 0; i < nChannels; ++i) {
        header.channels().insert(channelNames[i], Imf::Channel(Imf::FLOAT));
        frameBuffer.insert(channelNames[i], Imf::Slice(
            Imf::FLOAT, // Type
            (char*)(data.data() + i), // Base pointer
            sizeof(float) * nChannels, // x-stride in bytes
            sizeof(float) * imageSize.x() * nChannels // y-stride in bytes
        ));
    }

    StdOStream imfOStream{oStream, toString(path).c_str()};
    Imf::OutputFile file{imfOStream, header};
    file.setFrameBuffer(frameBuffer);
    file.writePixels(imageSize.y());
}

TEV_NAMESPACE_END
