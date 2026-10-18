using System.Diagnostics;
using System.IO.MemoryMappedFiles;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace Denpachan.Ui.Engine;

/// <summary>
/// worker が共有メモリに書くプレビュー画像 (BGRA 640x360、ダブルバッファ) を読む。
/// レイアウトは src/common/include/denpachan/preview_shm.h と合わせること。
/// 読んでいる間はハートビートを書き、worker はそれが途絶えたら縮小処理を止める。
/// </summary>
public sealed unsafe class PreviewReader : IDisposable
{
    private const int Width = 640;
    private const int Height = 360;
    private const int Stride = Width * 4;
    private const int FrameBytes = Stride * Height;
    private const int HeaderBytes = 4096;
    private const int Slots = 2;
    private const long MappingBytes = HeaderBytes + (long)FrameBytes * Slots;
    private const uint Magic = 0x56505044;  // "DPPV"
    private const int OffsetMagic = 0;
    private const int OffsetSeq = 24;
    private const int OffsetReaderHeartbeat = 32;
    private const int OffsetWriterQpc = 40;

    private readonly string _name;
    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _view;
    private ulong _lastSeq;

    public PreviewReader(int ch)
    {
        _name = $"Local\\denpachan_preview_{ch}";
        Bitmap = new WriteableBitmap(Width, Height, 96, 96, PixelFormats.Bgra32, null);
    }

    public WriteableBitmap Bitmap { get; }

    /// <summary>最後に新しい画像を受け取った時刻 (Stopwatch.GetTimestamp)</summary>
    public long LastFrameTimestamp { get; private set; }

    public bool HasFrame => _lastSeq != 0;

    /// <summary>新しい画像があれば Bitmap に写す。UI スレッドから呼ぶ。</summary>
    public bool Poll()
    {
        if (_view == null && !Open()) return false;
        var view = _view!;
        view.Write(OffsetReaderHeartbeat, Stopwatch.GetTimestamp());  // QPC (worker と同じ時計)
        if (view.ReadUInt32(OffsetMagic) != Magic) return false;
        var seq = view.ReadUInt64(OffsetSeq);
        if (seq == 0 || seq == _lastSeq) return false;

        byte* basePtr = null;
        view.SafeMemoryMappedViewHandle.AcquirePointer(ref basePtr);
        try
        {
            basePtr += view.PointerOffset;
            var slot = (int)(seq % Slots);
            Bitmap.WritePixels(new Int32Rect(0, 0, Width, Height), (IntPtr)(basePtr + HeaderBytes + (long)FrameBytes * slot),
                               FrameBytes, Stride);
        }
        finally
        {
            view.SafeMemoryMappedViewHandle.ReleasePointer();
        }
        // コピー中に 2 枚以上進んでいたら、そのスロットは上書きされている可能性がある (次で取り直す)
        if (view.ReadUInt64(OffsetSeq) - seq >= 2) return false;
        _lastSeq = seq;
        LastFrameTimestamp = view.ReadInt64(OffsetWriterQpc);
        return true;
    }

    private bool Open()
    {
        try
        {
            _mmf = MemoryMappedFile.CreateOrOpen(_name, MappingBytes, MemoryMappedFileAccess.ReadWrite);
            _view = _mmf.CreateViewAccessor(0, MappingBytes);
            return true;
        }
        catch
        {
            Dispose();
            return false;
        }
    }

    public void Dispose()
    {
        _view?.Dispose();
        _mmf?.Dispose();
        _view = null;
        _mmf = null;
    }
}
