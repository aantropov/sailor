using System;
using System.IO;
using Newtonsoft.Json.Linq;

namespace SailorEditor.Utility;

public class GlbExtractor
{
    public static bool ExtractTextureFromGLB(string filePath, int textureIndex, out MemoryStream outTextureStream)
    {
        outTextureStream = null;

        using (var file = File.OpenRead(filePath))
        {
            if (file.Length < 12)
            {
                Console.WriteLine("Failed to open file");
                return false;
            }

            var header = new byte[12];
            file.Read(header, 0, 12);

            uint magic = BitConverter.ToUInt32(header, 0);
            if (magic != 0x46546C67)
            {
                Console.WriteLine("Failed to extract texture from GLB, Invalid GLB magic");
                return false;
            }

            var jsonChunkHeader = new byte[8];
            file.Read(jsonChunkHeader, 0, 8);

            uint jsonChunkType = BitConverter.ToUInt32(jsonChunkHeader, 4);
            if (jsonChunkType != 0x4E4F534A)
            {
                Console.WriteLine("Failed to extract texture from GLB, Invalid JSON chunk");
                return false;
            }

            uint jsonChunkLength = BitConverter.ToUInt32(jsonChunkHeader, 0);
            var jsonChunk = new byte[jsonChunkLength];
            file.Read(jsonChunk, 0, (int)jsonChunkLength);

            var gltfJson = JObject.Parse(System.Text.Encoding.UTF8.GetString(jsonChunk));

            if (textureIndex < 0 || textureIndex >= gltfJson["textures"].Count())
            {
                Console.WriteLine("Failed to extract texture from GLB, Invalid texture index");
                return false;
            }

            int imageIndex = (int)gltfJson["textures"][textureIndex]["source"];
            int bufferViewIndex = (int)gltfJson["images"][imageIndex]["bufferView"];
            var bufferView = gltfJson["bufferViews"][bufferViewIndex];
            int byteOffset = (int)bufferView["byteOffset"];
            int byteLength = (int)bufferView["byteLength"];

            file.Seek(12 + 8 + jsonChunkLength, SeekOrigin.Begin);

            var binChunkHeader = new byte[8];
            file.Read(binChunkHeader, 0, 8);

            uint binChunkType = BitConverter.ToUInt32(binChunkHeader, 4);
            if (binChunkType != 0x004E4942)
            {
                Console.WriteLine("Failed to extract texture from GLB, Invalid BIN chunk");
                return false;
            }

            uint binChunkLength = BitConverter.ToUInt32(binChunkHeader, 0);
            if (byteOffset + byteLength > (int)binChunkLength)
            {
                Console.WriteLine("Failed to extract texture from GLB, Invalid buffer view");
                return false;
            }

            file.Seek(byteOffset, SeekOrigin.Current);
            var buffer = new byte[byteLength];
            file.Read(buffer, 0, byteLength);

            outTextureStream = new MemoryStream(buffer);

            return true;
        }
    }
}