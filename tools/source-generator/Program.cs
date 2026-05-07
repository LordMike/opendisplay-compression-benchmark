using System.Globalization;
using System.IO;
using System.Linq;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

const int MasterWidth = 2560;
const int MasterHeight = 1440;

var benchmarkRoot = FindBenchmarkRoot(AppContext.BaseDirectory);
var sourcesRoot = Path.Combine(benchmarkRoot, "image_sources");
var generatedOriginals = Path.Combine(sourcesRoot, "generated", "originals");
var photosOriginals = Path.Combine(sourcesRoot, "photos", "originals");
Directory.CreateDirectory(generatedOriginals);
Directory.CreateDirectory(photosOriginals);

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

var scenes = new (string Name, Action<DrawingContext, double, double> Draw)[]
{
    ("dashboard-mixed-density", DrawDashboard),
    ("status-panel-large-numbers", DrawStatusPanel),
    ("price-label-barcode-layout", DrawPriceLabel),
    ("price-tag-electronics-promo", DrawElectronicsPriceTag),
    ("price-tag-grocery-shelf", DrawGroceryPriceTag),
    ("price-tag-warehouse-markdown", DrawWarehousePriceTag),
    ("menu-board-text-heavy", DrawMenuBoard),
    ("floorplan-thin-lines", DrawFloorplan),
    ("text-glyph-density-grid", DrawTextGlyphGrid),
};

foreach (var scene in scenes)
{
    var image = Render(MasterWidth, MasterHeight, dc => scene.Draw(dc, MasterWidth, MasterHeight));
    SavePng(image, Path.Combine(generatedOriginals, $"{scene.Name}.png"));
}

ResizeFolder(
    inputFolder: generatedOriginals,
    outputRoot: Path.Combine(sourcesRoot, "generated"),
    resolutions: resolutions,
    fitMode: FitMode.Contain);

ResizeFolder(
    inputFolder: photosOriginals,
    outputRoot: Path.Combine(sourcesRoot, "photos"),
    resolutions: resolutions,
    fitMode: FitMode.Cover);

Console.WriteLine("Generated regular-use masters and resized variants.");

static void ResizeFolder(
    string inputFolder,
    string outputRoot,
    IReadOnlyList<(int Width, int Height)> resolutions,
    FitMode fitMode)
{
    if (!Directory.Exists(inputFolder)) return;

    var files = Directory
        .EnumerateFiles(inputFolder)
        .Where(path =>
            path.EndsWith(".png", StringComparison.OrdinalIgnoreCase) ||
            path.EndsWith(".jpg", StringComparison.OrdinalIgnoreCase) ||
            path.EndsWith(".jpeg", StringComparison.OrdinalIgnoreCase))
        .OrderBy(path => path, StringComparer.OrdinalIgnoreCase)
        .ToArray();

    foreach (var (width, height) in resolutions)
    {
        var outDir = Path.Combine(outputRoot, $"{width}x{height}");
        Directory.CreateDirectory(outDir);

        foreach (var file in files)
        {
            var source = LoadBitmap(file);
            var resized = Resize(source, width, height, fitMode);
            var name = Path.GetFileNameWithoutExtension(file) + ".png";
            SavePng(resized, Path.Combine(outDir, name));
        }
    }
}

static BitmapSource Resize(BitmapSource source, int width, int height, FitMode fitMode)
{
    var sourceW = source.PixelWidth;
    var sourceH = source.PixelHeight;
    var targetRatio = (double)width / height;
    var sourceRatio = (double)sourceW / sourceH;

    BitmapSource drawableSource = source;
    Rect dest;

    if (fitMode == FitMode.Cover)
    {
        Int32Rect crop;
        if (sourceRatio > targetRatio)
        {
            var cropW = (int)Math.Round(sourceH * targetRatio);
            var x = Math.Max(0, (sourceW - cropW) / 2);
            crop = new Int32Rect(x, 0, Math.Min(cropW, sourceW - x), sourceH);
        }
        else
        {
            var cropH = (int)Math.Round(sourceW / targetRatio);
            var y = Math.Max(0, (sourceH - cropH) / 2);
            crop = new Int32Rect(0, y, sourceW, Math.Min(cropH, sourceH - y));
        }

        drawableSource = new CroppedBitmap(source, crop);
        dest = new Rect(0, 0, width, height);
    }
    else
    {
        var scale = Math.Min((double)width / sourceW, (double)height / sourceH);
        var drawW = Math.Round(sourceW * scale);
        var drawH = Math.Round(sourceH * scale);
        dest = new Rect(Math.Floor((width - drawW) / 2), Math.Floor((height - drawH) / 2), drawW, drawH);
    }

    return Render(width, height, dc =>
    {
        dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, width, height));
        dc.DrawImage(drawableSource, dest);
    });
}

static BitmapSource LoadBitmap(string path)
{
    var bitmap = new BitmapImage();
    bitmap.BeginInit();
    bitmap.CacheOption = BitmapCacheOption.OnLoad;
    bitmap.UriSource = new System.Uri(Path.GetFullPath(path), UriKind.Absolute);
    bitmap.EndInit();
    bitmap.Freeze();
    return bitmap;
}

static RenderTargetBitmap Render(int width, int height, Action<DrawingContext> draw)
{
    var visual = new DrawingVisual();
    using (var dc = visual.RenderOpen())
    {
        draw(dc);
    }

    var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
    bitmap.Render(visual);
    bitmap.Freeze();
    return bitmap;
}

static void SavePng(BitmapSource bitmap, string path)
{
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    var encoder = new PngBitmapEncoder();
    encoder.Frames.Add(BitmapFrame.Create(bitmap));
    using var stream = File.Create(path);
    encoder.Save(stream);
}

static void DrawDashboard(DrawingContext dc, double w, double h)
{
    Background(dc, w, h);
    Header(dc, w, "HOUSE STATUS", "mixed density dashboard");

    var margin = 96.0;
    var top = 190.0;
    var gap = 32.0;
    var cardW = (w - margin * 2 - gap * 3) / 4;
    var cardH = 230.0;
    var labels = new[] { "TEMPERATURE", "HUMIDITY", "BATTERY", "POWER" };
    var values = new[] { "21.4", "44", "87", "312" };
    var units = new[] { "C", "%", "%", "W" };
    for (var i = 0; i < 4; i++)
    {
        var x = margin + i * (cardW + gap);
        Card(dc, x, top, cardW, cardH);
        Text(dc, labels[i], x + 32, top + 34, 32, Palette.Muted);
        Text(dc, values[i], x + 32, top + 72, 96, Palette.Black, FontWeights.Bold);
        Text(dc, units[i], x + cardW - 82, top + 112, 36, Palette.Red, FontWeights.Bold);
        Sparkline(dc, x + 32, top + 174, cardW - 64, 30, i * 13);
    }

    Card(dc, margin, 470, 1020, 790);
    Text(dc, "ENERGY TODAY", margin + 40, 516, 44, Palette.Black, FontWeights.Bold);
    BarChart(dc, margin + 62, 608, 920, 520);
    TextRows(dc, margin + 1130, 492, 1230, 740, 18, 34);

    Card(dc, margin + 1130, 470, 1230, 368);
    Text(dc, "ROOMS", margin + 1170, 520, 38, Palette.Black, FontWeights.Bold);
    MiniTable(dc, margin + 1170, 592, 1140, 196, 5);

    Card(dc, margin + 1130, 870, 1230, 390);
    Text(dc, "ALERTS", margin + 1170, 920, 38, Palette.Black, FontWeights.Bold);
    AlertList(dc, margin + 1170, 990, 1140, 210);
}

static void DrawStatusPanel(DrawingContext dc, double w, double h)
{
    Background(dc, w, h);
    Header(dc, w, "CLIMATE", "large number status panel");
    Text(dc, "21.8", 130, 280, 330, Palette.Black, FontWeights.Bold);
    Text(dc, "C", 910, 430, 100, Palette.Red, FontWeights.Bold);
    Text(dc, "INDOOR", 142, 650, 58, Palette.Muted, FontWeights.Bold);
    Text(dc, "Comfort band", 150, 735, 42, Palette.Black);
    Gauge(dc, 1320, 270, 780, 780);
    TextRows(dc, 150, 900, 980, 330, 7, 50);
}

static void DrawPriceLabel(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, w, h));
    dc.DrawRectangle(Palette.Black, null, new Rect(0, 0, w, 160));
    Text(dc, "PRODUCT LABEL", 86, 40, 64, Brushes.White, FontWeights.Bold);
    Text(dc, "ORGANIC OAT DRINK", 92, 250, 96, Palette.Black, FontWeights.Bold);
    Text(dc, "1 LITER  |  UNSWEETENED", 98, 374, 44, Palette.Muted);
    Text(dc, "$", 118, 560, 150, Palette.Black, FontWeights.Bold);
    Text(dc, "2", 240, 455, 420, Palette.Black, FontWeights.Bold);
    Text(dc, "49", 720, 512, 180, Palette.Black, FontWeights.Bold);
    Text(dc, "SAVE 20%", 1270, 530, 92, Palette.Red, FontWeights.Bold);
    Barcode(dc, 1320, 750, 930, 300);
    TextRows(dc, 118, 1130, 2160, 180, 3, 45);
}

static void DrawElectronicsPriceTag(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, w, h));

    dc.DrawRectangle(Palette.Red, null, new Rect(110, 120, 780, 370));
    Text(dc, "ACTION CAM", 165, 170, 110, Brushes.White, FontWeights.Bold);
    Text(dc, "WATERPROOF KIT", 170, 305, 54, Brushes.White, FontWeights.Bold);
    Text(dc, "SAVE 25%", 170, 392, 48, Brushes.White, FontWeights.Bold);
    Text(dc, "$", 135, 760, 130, Palette.Black, FontWeights.Bold);
    Text(dc, "129", 250, 645, 310, Palette.Black, FontWeights.Bold);
    Text(dc, "95", 830, 710, 136, Palette.Black, FontWeights.Bold);
    Barcode(dc, 150, 1060, 900, 130);

    DrawCameraProduct(dc, 1250, 260, 860, 430);
    QrCode(dc, 2140, 150, 148, 9);
    MicroStripe(dc, 2320, 150, 150, 136, 4);
    Text(dc, "SKU AC-4800-Y", 1265, 785, 44, Palette.Muted, FontWeights.Bold);
    Text(dc, "IP68  |  4K  |  128 GB CARD", 1265, 850, 42, Palette.Black);
    TextRows(dc, 1268, 950, 1040, 160, 4, 34);
}

static void DrawGroceryPriceTag(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(252, 252, 248)), null, new Rect(0, 0, w, h));

    dc.DrawRectangle(Palette.Black, null, new Rect(100, 110, 610, 160));
    Text(dc, "WEEKEND DEAL", 155, 152, 62, Brushes.White, FontWeights.Bold);
    Text(dc, "ROASTED COFFEE", 120, 360, 108, Palette.Black, FontWeights.Bold);
    Text(dc, "WHOLE BEAN 500 G", 126, 500, 48, Palette.Muted, FontWeights.Bold);
    Text(dc, "2 FOR", 132, 650, 72, Palette.Red, FontWeights.Bold);
    Text(dc, "$", 355, 650, 122, Palette.Black, FontWeights.Bold);
    Text(dc, "18", 470, 548, 318, Palette.Black, FontWeights.Bold);
    Text(dc, "00", 905, 615, 128, Palette.Black, FontWeights.Bold);

    DrawCoffeeBag(dc, 1320, 300, 520, 500);
    DrawCoffeeBag(dc, 1790, 345, 450, 430);
    Barcode(dc, 145, 1045, 930, 110);
    QrCode(dc, 2290, 180, 132, 8);
    TextRows(dc, 1290, 940, 1050, 120, 4, 30);
}

static void DrawWarehousePriceTag(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, w, h));
    dc.DrawRectangle(Palette.Red, null, new Rect(0, 0, 560, h));
    Text(dc, "CLEARANCE", 74, 110, 78, Brushes.White, FontWeights.Bold);
    Text(dc, "LAST UNITS", 78, 230, 52, Brushes.White, FontWeights.Bold);
    MicroStripe(dc, 82, 1040, 390, 115, 6);

    Text(dc, "SMART PLUG 4-PACK", 700, 135, 94, Palette.Black, FontWeights.Bold);
    Text(dc, "ENERGY MONITORING  |  WIFI", 706, 260, 42, Palette.Muted, FontWeights.Bold);
    Text(dc, "$", 700, 520, 138, Palette.Black, FontWeights.Bold);
    Text(dc, "34", 825, 405, 360, Palette.Black, FontWeights.Bold);
    Text(dc, "99", 1275, 485, 146, Palette.Black, FontWeights.Bold);
    Text(dc, "WAS $49.99", 710, 835, 62, Palette.Red, FontWeights.Bold);

    DrawPackageStack(dc, 1580, 300, 620, 405);
    Barcode(dc, 700, 1005, 950, 130);
    QrCode(dc, 2190, 940, 148, 10);
}

static void DrawMenuBoard(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(250, 249, 245)), null, new Rect(0, 0, w, h));
    Header(dc, w, "CAFE BOARD", "text heavy menu");
    var colW = (w - 240) / 3;
    var titles = new[] { "BREAKFAST", "LUNCH", "DRINKS" };
    for (var c = 0; c < 3; c++)
    {
        var x = 90 + c * (colW + 30);
        Text(dc, titles[c], x, 230, 58, Palette.Black, FontWeights.Bold);
        dc.DrawRectangle(Palette.Red, null, new Rect(x, 304, colW - 40, 10));
        for (var i = 0; i < 10; i++)
        {
            var y = 360 + i * 92;
            Text(dc, $"ITEM {c + 1}-{i + 1:00}", x, y, 34, Palette.Black, FontWeights.Bold);
            Text(dc, $"{7 + i}.{(c * 3 + i) % 10}0", x + colW - 190, y, 34, Palette.Black, FontWeights.Bold);
            Line(dc, x, y + 52, x + colW - 70, y + 52, Palette.Light, 3);
            Text(dc, "small description with dense glyph edges", x, y + 44, 22, Palette.Muted);
        }
    }
}

static void DrawFloorplan(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, w, h));
    Header(dc, w, "FLOORPLAN", "thin lines and annotations");
    var pen = new Pen(Palette.Black, 6);
    var thin = new Pen(Palette.Black, 2);
    var red = new Pen(Palette.Red, 5);
    var x0 = 170.0;
    var y0 = 250.0;
    var ww = 1850.0;
    var hh = 900.0;
    dc.DrawRectangle(null, pen, new Rect(x0, y0, ww, hh));
    Line(dc, x0 + 520, y0, x0 + 520, y0 + hh, Palette.Black, 5);
    Line(dc, x0 + 1130, y0, x0 + 1130, y0 + hh, Palette.Black, 5);
    Line(dc, x0, y0 + 420, x0 + 1130, y0 + 420, Palette.Black, 5);
    for (var i = 0; i < 48; i++)
    {
        var x = x0 + 50 + (i * 73 % 1740);
        var y = y0 + 55 + (i * 131 % 800);
        dc.DrawEllipse(null, thin, new Point(x, y), 18, 18);
        Line(dc, x - 28, y, x + 28, y, Palette.Light, 2);
        Line(dc, x, y - 28, x, y + 28, Palette.Light, 2);
    }
    for (var i = 0; i < 8; i++)
    {
        var x = x0 + 140 + i * 206;
        DrawArc(dc, red, new Point(x, y0 + 420), 66, 0, 90);
    }
    Text(dc, "KITCHEN", x0 + 80, y0 + 70, 42, Palette.Black, FontWeights.Bold);
    Text(dc, "LIVING", x0 + 630, y0 + 70, 42, Palette.Black, FontWeights.Bold);
    Text(dc, "UTILITY", x0 + 1240, y0 + 70, 42, Palette.Black, FontWeights.Bold);
    TextRows(dc, 2070, 270, 380, 770, 14, 38);
}

static void DrawTextGlyphGrid(DrawingContext dc, double w, double h)
{
    Background(dc, w, h);
    Header(dc, w, "GLYPH DENSITY", "small text and repeated symbols");
    var chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    var y = 220.0;
    for (var row = 0; row < 26; row++)
    {
        var x = 88.0;
        for (var col = 0; col < 34; col++)
        {
            var ch = chars[(row * 7 + col * 11) % chars.Length];
            Text(dc, ch.ToString(), x, y, 28 + (col % 3) * 4, (row + col) % 9 == 0 ? Palette.Red : Palette.Black, FontWeights.Bold);
            x += 70;
        }
        y += 43;
    }
    for (var i = 0; i < 18; i++)
    {
        Line(dc, 80, 220 + i * 64, w - 80, 220 + i * 64, Palette.Light, 1);
    }
}

static void DrawCameraProduct(DrawingContext dc, double x, double y, double w, double h)
{
    dc.DrawRoundedRectangle(Palette.Yellow, new Pen(Palette.Black, 5), new Rect(x, y + 60, w * 0.76, h * 0.62), 36, 36);
    dc.DrawRectangle(Palette.Black, null, new Rect(x + 42, y + 98, 138, 36));
    dc.DrawRoundedRectangle(Palette.Black, null, new Rect(x + 478, y + 92, 116, 84), 18, 18);
    dc.DrawEllipse(Palette.Black, null, new Point(x + 322, y + 178), 142, 142);
    dc.DrawEllipse(new SolidColorBrush(Color.FromRgb(50, 55, 60)), new Pen(Brushes.White, 5), new Point(x + 322, y + 178), 102, 102);
    dc.DrawEllipse(new SolidColorBrush(Color.FromRgb(12, 15, 18)), new Pen(Palette.Muted, 4), new Point(x + 322, y + 178), 70, 70);
    for (var i = 0; i < 62; i++)
    {
        var px = x + 22 + (i * 37 % 518);
        var py = y + 78 + (i * 61 % 208);
        dc.DrawEllipse(Palette.Black, null, new Point(px, py), 2, 2);
    }
}

static void DrawCoffeeBag(DrawingContext dc, double x, double y, double w, double h)
{
    var bag = new StreamGeometry();
    using (var ctx = bag.Open())
    {
        ctx.BeginFigure(new Point(x + w * 0.18, y + h), true, true);
        ctx.LineTo(new Point(x + w * 0.05, y + h * 0.18), true, false);
        ctx.LineTo(new Point(x + w * 0.28, y), true, false);
        ctx.LineTo(new Point(x + w * 0.82, y), true, false);
        ctx.LineTo(new Point(x + w * 0.95, y + h * 0.18), true, false);
        ctx.LineTo(new Point(x + w * 0.82, y + h), true, false);
    }
    bag.Freeze();
    dc.DrawGeometry(Palette.Red, new Pen(Palette.Black, 5), bag);
    dc.DrawRectangle(Brushes.White, null, new Rect(x + w * 0.20, y + h * 0.25, w * 0.60, h * 0.34));
    Text(dc, "COFFEE", x + w * 0.25, y + h * 0.33, w * 0.095, Palette.Black, FontWeights.Bold);
    for (var i = 0; i < 42; i++)
    {
        dc.DrawEllipse(Palette.Black, null, new Point(x + w * (0.18 + (i * 17 % 61) / 100.0), y + h * (0.67 + (i * 23 % 24) / 100.0)), 6, 4);
    }
}

static void DrawPackageStack(DrawingContext dc, double x, double y, double w, double h)
{
    for (var i = 0; i < 4; i++)
    {
        var xx = x + (i % 2) * w * 0.33;
        var yy = y + (i / 2) * h * 0.43;
        dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(242, 244, 246)), new Pen(Palette.Black, 4), new Rect(xx, yy, w * 0.44, h * 0.36));
        dc.DrawRectangle(Palette.Red, null, new Rect(xx + 28, yy + 26, w * 0.18, h * 0.09));
        Text(dc, $"P{i + 1:00}", xx + 34, yy + 86, 36, Palette.Black, FontWeights.Bold);
        MicroStripe(dc, xx + w * 0.25, yy + 32, w * 0.13, h * 0.22, 3);
    }
}

static void QrCode(DrawingContext dc, double x, double y, double size, int modules)
{
    dc.DrawRectangle(Brushes.White, new Pen(Palette.Black, 4), new Rect(x, y, size, size));
    var cell = size / modules;
    for (var row = 0; row < modules; row++)
    {
        for (var col = 0; col < modules; col++)
        {
            var finder = (row < 3 && col < 3) || (row < 3 && col >= modules - 3) || (row >= modules - 3 && col < 3);
            var filled = finder || ((row * 13 + col * 19 + row * col) % 7 < 3);
            if (filled)
            {
                dc.DrawRectangle(Palette.Black, null, new Rect(x + col * cell, y + row * cell, Math.Ceiling(cell), Math.Ceiling(cell)));
            }
        }
    }
    dc.DrawRectangle(Brushes.White, null, new Rect(x + cell, y + cell, cell, cell));
    dc.DrawRectangle(Brushes.White, null, new Rect(x + size - cell * 2, y + cell, cell, cell));
    dc.DrawRectangle(Brushes.White, null, new Rect(x + cell, y + size - cell * 2, cell, cell));
}

static void MicroStripe(DrawingContext dc, double x, double y, double w, double h, int step)
{
    dc.DrawRectangle(Brushes.White, new Pen(Palette.Black, 3), new Rect(x, y, w, h));
    for (var i = 0; i < w; i += step)
    {
        var height = h * (0.35 + ((i * 17) % 55) / 100.0);
        dc.DrawRectangle((i / step) % 3 == 0 ? Palette.Black : Palette.Muted, null, new Rect(x + i, y + h - height, Math.Max(1, step - 1), height));
    }
}

static void Background(DrawingContext dc, double w, double h)
{
    dc.DrawRectangle(new SolidColorBrush(Color.FromRgb(246, 247, 248)), null, new Rect(0, 0, w, h));
}

static void Header(DrawingContext dc, double w, string title, string subtitle)
{
    dc.DrawRectangle(Brushes.White, null, new Rect(0, 0, w, 150));
    dc.DrawRectangle(Palette.Black, null, new Rect(0, 146, w, 4));
    Text(dc, title, 86, 38, 58, Palette.Black, FontWeights.Bold);
    Text(dc, subtitle, w - 760, 58, 32, Palette.Muted);
}

static void Card(DrawingContext dc, double x, double y, double w, double h)
{
    dc.DrawRoundedRectangle(Brushes.White, new Pen(Palette.Light, 3), new Rect(x, y, w, h), 12, 12);
}

static void TextRows(DrawingContext dc, double x, double y, double w, double h, int rows, double rowHeight)
{
    for (var i = 0; i < rows; i++)
    {
        var yy = y + i * rowHeight;
        var ww = w * (0.45 + ((i * 37) % 50) / 100.0);
        dc.DrawRectangle((i % 5 == 0) ? Palette.Red : Palette.Black, null, new Rect(x, yy, ww, Math.Max(5, rowHeight * 0.18)));
        dc.DrawRectangle(Palette.Light, null, new Rect(x, yy + rowHeight * 0.42, w * (0.35 + ((i * 19) % 45) / 100.0), Math.Max(3, rowHeight * 0.12)));
    }
}

static void Sparkline(DrawingContext dc, double x, double y, double w, double h, int seed)
{
    var geo = new StreamGeometry();
    using (var ctx = geo.Open())
    {
        for (var i = 0; i < 28; i++)
        {
            var px = x + i * w / 27;
            var py = y + h * (0.2 + (((i * 17 + seed) % 23) / 28.0));
            if (i == 0) ctx.BeginFigure(new Point(px, py), false, false);
            else ctx.LineTo(new Point(px, py), true, false);
        }
    }
    geo.Freeze();
    dc.DrawGeometry(null, new Pen(Palette.Black, 4), geo);
}

static void BarChart(DrawingContext dc, double x, double y, double w, double h)
{
    for (var i = 0; i < 24; i++)
    {
        var bh = h * (0.12 + ((i * 29) % 74) / 100.0);
        var bw = w / 32;
        var bx = x + i * w / 24;
        dc.DrawRectangle(i % 6 == 0 ? Palette.Red : Palette.Black, null, new Rect(bx, y + h - bh, bw, bh));
    }
}

static void MiniTable(DrawingContext dc, double x, double y, double w, double h, int rows)
{
    for (var i = 0; i < rows; i++)
    {
        var yy = y + i * h / rows;
        Text(dc, $"ZONE {i + 1}", x, yy, 30, Palette.Black, FontWeights.Bold);
        dc.DrawRectangle(i % 2 == 0 ? Palette.Red : Palette.Black, null, new Rect(x + w - 210, yy + 8, 160, 22));
        Line(dc, x, yy + 48, x + w, yy + 48, Palette.Light, 2);
    }
}

static void AlertList(DrawingContext dc, double x, double y, double w, double h)
{
    for (var i = 0; i < 4; i++)
    {
        var yy = y + i * 54;
        dc.DrawEllipse(i == 0 ? Palette.Red : Palette.Black, null, new Point(x + 16, yy + 18), 12, 12);
        Text(dc, $"EVENT {i + 1} PENDING IN SYSTEM QUEUE", x + 48, yy, 30, Palette.Black);
    }
}

static void Gauge(DrawingContext dc, double x, double y, double w, double h)
{
    dc.DrawEllipse(null, new Pen(Palette.Light, 48), new Point(x + w / 2, y + h / 2), w / 2 - 40, h / 2 - 40);
    DrawArc(dc, new Pen(Palette.Red, 48), new Point(x + w / 2, y + h / 2), w / 2 - 40, -210, 250);
    Text(dc, "44%", x + 260, y + 315, 110, Palette.Black, FontWeights.Bold);
}

static void Barcode(DrawingContext dc, double x, double y, double w, double h)
{
    var cursor = x;
    for (var i = 0; i < 96 && cursor < x + w; i++)
    {
        var bw = 4 + (i * 17 % 7) * 3;
        if ((i * 11 % 5) != 0)
        {
            dc.DrawRectangle(Palette.Black, null, new Rect(cursor, y, bw, h));
        }
        cursor += bw + 5;
    }
}

static void Line(DrawingContext dc, double x1, double y1, double x2, double y2, Brush brush, double thickness)
{
    dc.DrawLine(new Pen(brush, thickness), new Point(x1, y1), new Point(x2, y2));
}

static void Text(DrawingContext dc, string text, double x, double y, double size, Brush brush, FontWeight? weight = null)
{
    var formatted = new FormattedText(
        text,
        CultureInfo.InvariantCulture,
        FlowDirection.LeftToRight,
        new Typeface(new FontFamily("Segoe UI"), FontStyles.Normal, weight ?? FontWeights.Normal, FontStretches.Normal),
        size,
        brush,
        1.0);
    dc.DrawText(formatted, new Point(x, y));
}

static void DrawArc(DrawingContext dc, Pen pen, Point center, double radius, double startDeg, double sweepDeg)
{
    var start = PointOnCircle(center, radius, startDeg);
    var end = PointOnCircle(center, radius, startDeg + sweepDeg);
    var geo = new StreamGeometry();
    using (var ctx = geo.Open())
    {
        ctx.BeginFigure(start, false, false);
        ctx.ArcTo(end, new Size(radius, radius), 0, Math.Abs(sweepDeg) > 180, sweepDeg > 0 ? SweepDirection.Clockwise : SweepDirection.Counterclockwise, true, false);
    }
    geo.Freeze();
    dc.DrawGeometry(null, pen, geo);
}

static Point PointOnCircle(Point center, double radius, double degrees)
{
    var radians = degrees * Math.PI / 180;
    return new Point(center.X + Math.Cos(radians) * radius, center.Y + Math.Sin(radians) * radius);
}

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

enum FitMode
{
    Contain,
    Cover,
}

static class Palette
{
    public static readonly Brush Black = new SolidColorBrush(Color.FromRgb(18, 20, 22));
    public static readonly Brush Muted = new SolidColorBrush(Color.FromRgb(88, 96, 104));
    public static readonly Brush Light = new SolidColorBrush(Color.FromRgb(208, 214, 220));
    public static readonly Brush Red = new SolidColorBrush(Color.FromRgb(200, 24, 32));
    public static readonly Brush Yellow = new SolidColorBrush(Color.FromRgb(244, 174, 18));
}
