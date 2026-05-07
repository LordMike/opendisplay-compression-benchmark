using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

var benchmarkRoot = FindBenchmarkRoot(AppContext.BaseDirectory);
var outputRoot = Path.Combine(benchmarkRoot, "image_sources", "stress");
Directory.CreateDirectory(outputRoot);

var resolutions = new (int Width, int Height)[]
{
    (2560, 1440),
    (1872, 1404),
    (1600, 1200),
    (1360, 480),
    (1024, 576),
    (960, 640),
    (800, 480),
    (648, 480),
    (600, 448),
    (400, 300),
    (296, 128),
    (200, 200),
    (152, 296),
    (128, 296),
    (122, 250),
};

var variants = new Variant[]
{
    new("checkerboard-mono-1px",       (x, y, w, h) => Checker(Palettes.Mono, x, y, 1)),
    new("checkerboard-mono-2px",       (x, y, w, h) => Checker(Palettes.Mono, x, y, 2)),
    new("checkerboard-bwr-1px",        (x, y, w, h) => Checker(Palettes.Bwr, x, y, 1)),
    new("checkerboard-bwy-1px",        (x, y, w, h) => Checker(Palettes.Bwy, x, y, 1)),
    new("checkerboard-bwry-1px",       (x, y, w, h) => Checker(Palettes.Bwry, x, y, 1)),
    new("checkerboard-bwgbry-1px",     (x, y, w, h) => Checker(Palettes.Bwgbry, x, y, 1)),
    new("checkerboard-gray16-1px",     (x, y, w, h) => Checker(Palettes.Gray16, x, y, 1)),

    new("random-noise-mono",           (x, y, w, h) => Noise(Palettes.Mono, x, y, 0x00C0FFEE)),
    new("random-noise-bwr",            (x, y, w, h) => Noise(Palettes.Bwr, x, y, 0x00B00B1E)),
    new("random-noise-bwy",            (x, y, w, h) => Noise(Palettes.Bwy, x, y, 0x00F00D42)),
    new("random-noise-bwry",           (x, y, w, h) => Noise(Palettes.Bwry, x, y, 0x005EED01)),
    new("random-noise-bwgbry",         (x, y, w, h) => Noise(Palettes.Bwgbry, x, y, 0x005EED02)),
    new("random-noise-gray16",         (x, y, w, h) => Noise(Palettes.Gray16, x, y, 0x005EED10)),

    new("diagonal-hatch-multi-angle-mono", (x, y, w, h) => DiagonalHatchMono(x, y)),
    new("color-plane-interleave-bwr",      (x, y, w, h) => ColorPlaneInterleave(Palettes.Bwr, x, y)),
    new("color-plane-interleave-bwy",      (x, y, w, h) => ColorPlaneInterleave(Palettes.Bwy, x, y)),
    new("color-plane-interleave-bwry",     (x, y, w, h) => ColorPlaneInterleave(Palettes.Bwry, x, y)),
    new("mixed-entropy-bands-gray16",      MixedEntropyBandsGray16),
    new("smooth-gradient-dither-trap",     SmoothGradientTrap),
};

foreach (var (width, height) in resolutions)
{
    var resolutionDir = Path.Combine(outputRoot, $"{width}x{height}");
    Directory.CreateDirectory(resolutionDir);

    foreach (var variant in variants)
    {
        var path = Path.Combine(resolutionDir, $"{variant.Name}.png");
        PngWriter.WriteRgb(path, width, height, variant.Pixel);
    }
}

Console.WriteLine($"Generated {resolutions.Length * variants.Length} stress images in {outputRoot}");

static string FindBenchmarkRoot(string start)
{
    var dir = new DirectoryInfo(start);
    while (dir is not null)
    {
        if (Directory.Exists(Path.Combine(dir.FullName, "image_sources")) &&
            Directory.Exists(Path.Combine(dir.FullName, "tools")))
        {
            return dir.FullName;
        }

        dir = dir.Parent;
    }

    return Directory.GetCurrentDirectory();
}

static Rgb Checker(Rgb[] palette, int x, int y, int blockSize)
{
    return palette[((x / blockSize) + (y / blockSize)) % palette.Length];
}

static Rgb Noise(Rgb[] palette, int x, int y, uint seed)
{
    var h = Hash((uint)x, (uint)y, seed);
    return palette[h % palette.Length];
}

static Rgb DiagonalHatchMono(int x, int y)
{
    var a = ((x + y) % 11) < 3;
    var b = ((x * 2 - y + 8192) % 17) < 4;
    var c = ((x - y * 3 + 8192) % 23) < 5;
    return (a ^ b ^ c) ? Palettes.Black : Palettes.White;
}

static Rgb ColorPlaneInterleave(Rgb[] palette, int x, int y)
{
    var tileX = x & 3;
    var tileY = y & 3;
    var index = (tileX + tileY * 3) % palette.Length;
    return palette[index];
}

static Rgb MixedEntropyBandsGray16(int x, int y, int width, int height)
{
    var band = y * 5 / height;
    return band switch
    {
        0 => Palettes.White,
        1 => ((x / 12 + y / 16) % 2 == 0) ? Palettes.Gray16[2] : Palettes.Gray16[14],
        2 => Noise(Palettes.Gray16, x, y, 0x00A11CE5),
        3 => Palettes.Gray16[(x * 15) / Math.Max(1, width - 1)],
        _ => Checker(Palettes.Gray16, x, y, 3),
    };
}

static Rgb SmoothGradientTrap(int x, int y, int width, int height)
{
    var r = (byte)(x * 255 / Math.Max(1, width - 1));
    var g = (byte)(y * 255 / Math.Max(1, height - 1));
    var b = (byte)(((x + y) * 255) / Math.Max(1, width + height - 2));
    return new Rgb(r, g, b);
}

static int Hash(uint x, uint y, uint seed)
{
    var v = x * 0x9E3779B1u ^ y * 0x85EBCA77u ^ seed;
    v ^= v >> 16;
    v *= 0x7FEB352Du;
    v ^= v >> 15;
    v *= 0x846CA68Bu;
    v ^= v >> 16;
    return unchecked((int)(v & 0x7FFFFFFF));
}

delegate Rgb PixelFunc(int x, int y, int width, int height);

readonly record struct Variant(string Name, PixelFunc Pixel);

readonly record struct Rgb(byte R, byte G, byte B);

static class Palettes
{
    public static readonly Rgb Black = new(0, 0, 0);
    public static readonly Rgb White = new(255, 255, 255);
    public static readonly Rgb Red = new(220, 0, 0);
    public static readonly Rgb Yellow = new(255, 216, 0);
    public static readonly Rgb Green = new(0, 170, 70);
    public static readonly Rgb Blue = new(0, 85, 210);

    public static readonly Rgb[] Mono = { Black, White };
    public static readonly Rgb[] Bwr = { Black, White, Red };
    public static readonly Rgb[] Bwy = { Black, White, Yellow };
    public static readonly Rgb[] Bwry = { Black, White, Red, Yellow };
    public static readonly Rgb[] Bwgbry = { Black, White, Green, Blue, Red, Yellow };
    public static readonly Rgb[] Gray16 = Enumerable.Range(0, 16)
        .Select(i => new Rgb((byte)(i * 17), (byte)(i * 17), (byte)(i * 17)))
        .ToArray();
}

static class PngWriter
{
    private static readonly byte[] Signature = { 137, 80, 78, 71, 13, 10, 26, 10 };

    public static void WriteRgb(string path, int width, int height, PixelFunc pixel)
    {
        using var file = File.Create(path);
        file.Write(Signature);

        Span<byte> ihdr = stackalloc byte[13];
        BinaryPrimitives.WriteInt32BigEndian(ihdr[..4], width);
        BinaryPrimitives.WriteInt32BigEndian(ihdr.Slice(4, 4), height);
        ihdr[8] = 8; // bit depth
        ihdr[9] = 2; // truecolor RGB
        ihdr[10] = 0;
        ihdr[11] = 0;
        ihdr[12] = 0;
        WriteChunk(file, "IHDR", ihdr);

        using var compressed = new MemoryStream();
        using (var zlib = new ZLibStream(compressed, CompressionLevel.Fastest, leaveOpen: true))
        {
            var row = new byte[1 + width * 3];
            for (var y = 0; y < height; y++)
            {
                row[0] = 0; // PNG filter: none
                var offset = 1;
                for (var x = 0; x < width; x++)
                {
                    var rgb = pixel(x, y, width, height);
                    row[offset++] = rgb.R;
                    row[offset++] = rgb.G;
                    row[offset++] = rgb.B;
                }

                zlib.Write(row);
            }
        }

        WriteChunk(file, "IDAT", compressed.ToArray());
        WriteChunk(file, "IEND", ReadOnlySpan<byte>.Empty);
    }

    private static void WriteChunk(Stream stream, string type, ReadOnlySpan<byte> data)
    {
        Span<byte> length = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(length, data.Length);
        stream.Write(length);

        var typeBytes = Encoding.ASCII.GetBytes(type);
        stream.Write(typeBytes);
        stream.Write(data);

        var crc = Crc32.Compute(typeBytes, data);
        Span<byte> crcBytes = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(crcBytes, crc);
        stream.Write(crcBytes);
    }
}

static class Crc32
{
    private static readonly uint[] Table = BuildTable();

    public static uint Compute(ReadOnlySpan<byte> type, ReadOnlySpan<byte> data)
    {
        var crc = 0xFFFFFFFFu;
        crc = Update(crc, type);
        crc = Update(crc, data);
        return crc ^ 0xFFFFFFFFu;
    }

    private static uint Update(uint crc, ReadOnlySpan<byte> bytes)
    {
        foreach (var b in bytes)
        {
            crc = Table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        }

        return crc;
    }

    private static uint[] BuildTable()
    {
        var table = new uint[256];
        for (uint i = 0; i < table.Length; i++)
        {
            var c = i;
            for (var k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }

            table[i] = c;
        }

        return table;
    }
}
