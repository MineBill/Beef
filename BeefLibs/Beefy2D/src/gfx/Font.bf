using System;
using System.Interop;
using System.Collections;
using System.Text;
using System.IO;
using Beefy.geom;
using Beefy.utils;
using System.Diagnostics;
using System.Threading;
using res;

namespace Beefy.gfx
{
    public enum FontOverflowMode
    {
        Overflow,
        Clip,
        Truncate,
        Wrap,
        Ellipsis
    }

    public enum FontAlign
    {
        Left = -1,
        Centered,
        Right
    }

    public struct FontMetrics
    {
        public int32 mLineCount;
        public float mMinX;
        public float mMinY;
        public float mMaxX;
        public float mMaxY;

        public float mMaxWidth;
    }

	public class FontEffect
	{
		public enum Kind
		{
			case None;
			case Outline(float thickness, uint32 color);
		}

		// One font's effect glyphs, keyed by the source glyph. Dropped when the font frees its glyphs (Forget): a glyph
		// allocated later at a freed glyph's address would otherwise find another character's effect image.
		public class FontEntry
		{
			public ImageAtlas mAtlas = new ImageAtlas() ~ delete _;
			public Dictionary<Font.CharData, Font.CharData> mCharData = new .() ~ DeleteDictionaryAndValues!(_);
		}

		public Kind mKind;
		public String mEffectOptions = new .() ~ delete _;
		public Dictionary<Font, FontEntry> mFontEntries = new .() ~ DeleteDictionaryAndValues!(_);

		public this(Kind kind)
		{
			mKind = kind;
			switch (mKind)
			{
			case .Outline(let thickness, let color):
				mEffectOptions..Clear().AppendF(
					$"""
					Effect=Stroke
					Size={thickness}
					Color=#{color:X}
					""");
			default:
			}
		}

		public ~this()
		{
			for (var font in mFontEntries.Keys)
				font.[Friend]mEffectUsers.Remove(this);
		}

		public Font.CharData Apply(Font font, Font.CharData charData)
		{
			FontEntry fontEntry = null;
			if (mFontEntries.TryAdd(font, ?, var fontEntryPtr))
			{
				fontEntry = new .();
				*fontEntryPtr = fontEntry;
				if (font.[Friend]mEffectUsers == null)
					font.[Friend]mEffectUsers = new .();
				font.[Friend]mEffectUsers.Add(this);
			}
			else
				fontEntry = *fontEntryPtr;

			if (!fontEntry.mCharData.TryAdd(charData, ?, var effectCharDataPtr))
				return *effectCharDataPtr;
			let effectCharData = new Font.CharData(charData);
			*effectCharDataPtr = effectCharData;

			int32 ofsX = 0;
			int32 ofsY = 0;
			if (mKind case .Outline(let thickness, ?))
			{
				ofsX += (.)Math.Ceiling(thickness) + 2;
				ofsY += (.)Math.Ceiling(thickness) + 2;
			}

			effectCharData.mXOffset -= ofsX;
			effectCharData.mYOffset -= ofsY;
			// Drawn wherever the glyph is, so it needs the glyph's guard (copied into effectCharData above).
			var effectImage = fontEntry.mAtlas.Alloc(charData.mImageSegment.mSrcWidth + ofsX * 2, charData.mImageSegment.mSrcHeight + ofsY * 2, charData.mGuard);
			effectCharData.mImageSegment = effectImage;
			charData.mImageSegment.ApplyEffect(effectImage, mEffectOptions);
			return effectCharData;
		}

		// The font is freeing its glyphs (see Font.Dispose).
		public void Forget(Font font)
		{
			if (mFontEntries.GetAndRemove(font) case .Ok(let entry))
				delete entry.value;
		}

		public void Prepare(Font font, StringView str)
		{

		}
	}

    public class Font
    {
		[CallingConvention(.Stdcall), CLink]
		static extern FTFont* FTFont_Load(char8* fileName, float pointSize);

		[CallingConvention(.Stdcall), CLink]
		static extern void FTFont_Delete(FTFont* ftFont, bool cacheRetain);

		[CallingConvention(.Stdcall), CLink]
		static extern void FTFont_ClearCache();

		[CallingConvention(.Stdcall), CLink]
		static extern FTGlyph* FTFont_AllocGlyph(FTFont* ftFont, int32 char8Code, bool allowDefault);

		[CallingConvention(.Stdcall), CLink]
		static extern FTGlyph* FTFont_AllocGlyphGuarded(FTFont* ftFont, int32 char8Code, bool allowDefault, int32 guard);

		[CallingConvention(.Stdcall), CLink]
		static extern FTGlyph* FTFont_AllocGlyphGuardedSubpixel(FTFont* ftFont, int32 char8Code, bool allowDefault, int32 guard);

		[CallingConvention(.Stdcall), CLink]
		static extern int32 FTFont_GetKerning(FTFont* font, int32 char8CodeA, int32 char8CodeB);

		// Asks FreeType for a font file's real family/style names (see the C++ side). Returns the face count,
		// or 0 when the file can't be opened.
		[CallingConvention(.Stdcall), CLink]
		static extern int32 FTFont_GetFaceInfo(char8* fileName, int32 faceIndex, char8* outFamilyName, int32 familyNameBufSize,
			char8* outStyleName, int32 styleNameBufSize, int32* outFlags, int32* outWeight);

		static Dictionary<String, String> sFontNameMap ~ DeleteDictionaryAndKeysAndValues!(_);
		static Dictionary<String, String> sFontFailMap ~ DeleteDictionaryAndKeysAndValues!(_);
		static Monitor sMonitor = new .() ~ delete _;

		// Whether newly-loaded fonts rasterize with subpixel (LCD/RGB) antialiasing. The display's subpixel
		// order is only assumed on Windows; elsewhere the safe default is grayscale. Set before loading (eg
		// from an editor setting). A font still falls back to grayscale per-draw when its text is scaled or
		// rotated, where the RGB-stripe assumption doesn't hold.
#if BF_PLATFORM_WINDOWS
		public static bool sSubpixel = true;
#else
		public static bool sSubpixel = false;
#endif

		struct FTFont
		{
			public int32 mHeight;
			public int32 mAscent;
			public int32 mDescent;
			public int32 mMaxAdvance;
		}

		struct FTGlyph
		{
			public void* mPage;
			public void* mTextureSegment;
			public int32 mX;
			public int32 mY;
			public int32 mWidth;
			public int32 mHeight;
			public int32 mXOffset;
			public int32 mYOffset;
			public int32 mXAdvance;
		}

        public class CharData
        {
            public Image mImageSegment ~ delete _;
			// The subpixel (LCD/RGB) variant of mImageSegment, present only when the owning font is subpixel.
			// Drawn only for axis-aligned, unscaled text, and positioned with mXOffsetSubpixel/mYOffsetSubpixel
			// (LCD adds filter margin, so its extents/bearing differ from the grayscale image). Deliberately
			// not carried by the copy constructor: font effects always render through the grayscale image.
			public Image mImageSegmentSubpixel ~ delete _;
            public int32 mX;
            public int32 mY;
            public int32 mWidth;
            public int32 mHeight;
            public int32 mXOffset;
            public int32 mYOffset;
            public int32 mXAdvance;
			// Bitmap bearings for the subpixel image. LCD rasterization adds ~1px of filter margin per side
			// and shifts the bitmap's left bearing to match, so the subpixel image must be positioned with
			// these rather than mXOffset/mYOffset (the advances are identical, so only the ink offset differs).
			public int32 mXOffsetSubpixel;
			public int32 mYOffsetSubpixel;
			public bool mIsCombiningMark;
			// The guard requested for mImageSegment and anything made from it (Font.mGlyphGuard). Kept even for an
			// empty glyph: its effect image still has texels to guard.
			public int32 mGuard;

			public this()
			{

			}

			public this(CharData copyFrom)
			{
				mImageSegment = copyFrom.mImageSegment;
				mX = copyFrom.mX;
				mY = copyFrom.mY;
				mWidth = copyFrom.mWidth;
				mHeight = copyFrom.mHeight;
				mXOffset = copyFrom.mXOffset;
				mYOffset = copyFrom.mYOffset;
				mXAdvance = copyFrom.mXAdvance;
				mXOffsetSubpixel = copyFrom.mXOffsetSubpixel;
				mYOffsetSubpixel = copyFrom.mYOffsetSubpixel;
				mIsCombiningMark = copyFrom.mIsCombiningMark;
				mGuard = copyFrom.mGuard;
			}
        }

        public class Page
        {
            public Image mImage ~ delete _;
            public bool mIsCorrectedImage = true;
        }

		public struct AltFont
		{
			public Font mFont;
			public bool mOwned;
		}

		class MarkRefData
		{
			public float mTop;
			public float mBottom;
		}

		// One font face discovered while building the name cache: its real family/style (from the font itself,
		// via FreeType, or from fontconfig on Linux), the path to load it (with "@index" for a collection face),
		// and a style rank so the regular face can win a plain family lookup.
		class FontFaceInfo
		{
			public String mFamily ~ delete _;
			public String mStyle ~ delete _;
			// Alias the OS reports for this face (Windows registry value name), which may spell the style
			// differently than the font does (eg registry "Cascadia Mono Regular" vs face style "Roman").
			public String mDisplayName ~ delete _;
			public String mPath ~ delete _;
			public bool mRegular;
		}

		public class BitmapPage
		{
			public Image mImage ~ delete _;
		}

		enum MarkPosition
		{
			AboveC, // Center
			AboveR, // Left edge of mark aligned on center of char
			AboveE, // Center of mark aligned on right edge of char
			BelowC,
			BelowR,
			OverC,
			OverE,
			TopR, // Center of edge aligned to top of char
		}

        const int32 LOW_CHAR_COUNT = 128;

        Dictionary<char32, CharData> mCharData;
		List<BitmapPage> mBitmapPages ~ DeleteContainerAndItems!(_);
        CharData[] mLowCharData;
		FTFont* mFTFont;
		String mPath;
		List<AltFont> mAlternates;
		MarkRefData mMarkRefData ~ delete _;
		float[] mLoKerningTable;
		//BitmapFont mBMFont ~ delete _;
		public StringView mEllipsis = "...";
		// Empty texels kept around each glyph in its page, so a filtered (scaled or rotated) draw never samples a
		// neighbour. 1 covers bilinear filtering, except minified draws into a multisampled target, whose edge
		// pixels sample up to half a pixel outside the quad. Set by Load, before any glyph exists.
		int32 mGlyphGuard;
		// Captured from Font.sSubpixel at Load; fonts loaded from a .fnt bitmap atlas stay grayscale.
		public bool mSubpixel;

		List<FontEffect> mFontEffectStack ~ delete _;
		DisposeProxy mFontEffectDisposeProxy = new .(new () => { PopFontEffect(); }) ~ delete _;
		// Effects holding glyphs made from this font's, told when the glyphs go (see FontEffect.Forget).
		List<FontEffect> mEffectUsers ~ delete _;

#if BF_PLATFORM_LINUX

		const String FONTCONFIG_LIB = "libfontconfig.so";
		static bool IsFontconfigAvailable { get; private set; }

		[AlwaysInclude, StaticInitPriority(100)]
		private static class FontconfigAllowFail
		{
			static this()
			{
				SelfOuter.IsFontconfigAvailable = true;
				Runtime.AddErrorHandler(new (stage, error) => {
					if (stage == .PreFail)
					{
						if (let err = error as Runtime.LoadSharedLibraryError && err.mPath == FONTCONFIG_LIB)
						{
							SelfOuter.IsFontconfigAvailable = false;
							return .Ignore;
						}
					}

					return .ContinueFailure;
				});
			}
		}

		struct FcConfig;
		struct FcPattern;
		struct FcObjectSet;

		[CRepr]
		struct FcFontSet
		{
			public c_int nfont;
			public c_int sfont;
			public FcPattern** fonts;
		}

		enum FcResult : c_int
		{
			Match,
			NoMatch,
			TypeMismatch,
			NoId,
			OutOfMemory
		}

		const String FC_FAMILY = "family";
		const String FC_STYLE = "style";
		const String FC_WEIGHT = "weight";
		const String FC_SLANT = "slant";
		const String FC_FILE = "file";
		const String FC_INDEX = "index";

		[CLink, Import(FONTCONFIG_LIB)]
		static extern c_int FcInit();
		[CLink, Import(FONTCONFIG_LIB)]
		static extern void FcFini();

		[CLink, Import(FONTCONFIG_LIB)]
		static extern FcPattern* FcPatternCreate();
		[CLink, Import(FONTCONFIG_LIB)]
		static extern void FcPatternDestroy(FcPattern* p);

		[CLink, Import(FONTCONFIG_LIB)]
		static extern FcObjectSet* FcObjectSetBuild(c_char* first, ...);
		[CLink, Import(FONTCONFIG_LIB)]
		static extern void FcObjectSetDestroy(FcObjectSet* os);

		[CLink, Import(FONTCONFIG_LIB)]
		static extern FcFontSet* FcFontList(FcConfig* config, FcPattern* p, FcObjectSet* os);
		[CLink, Import(FONTCONFIG_LIB)]
		static extern void FcFontSetDestroy(FcFontSet* fs);

		[CLink, Import(FONTCONFIG_LIB)]
		static extern FcResult FcPatternGetString(FcPattern* p, c_char* object, c_int n, char8** s);

		[CLink, Import(FONTCONFIG_LIB)]
		static extern FcResult FcPatternGetInteger(FcPattern* p, c_char* object, c_int n, c_int *i);

#endif

        public this()
        {
        }

		public ~this()
		{
			Dispose();
		}

		public static ~this()
		{
			FTFont_ClearCache();
		}

		public DisposeProxy PushFontEffect(FontEffect fontEffect)
		{
			if (mFontEffectStack == null)
				mFontEffectStack = new .();
			mFontEffectStack.Add(fontEffect);
			return mFontEffectDisposeProxy;
		}

		public void PopFontEffect()
		{
			mFontEffectStack.PopBack();
		}

		// Resolves a font path value (which may be a bare file name under the Windows fonts folder) into an
		// absolute path, appended to outPath.
		static void ResolveFontPath(StringView fontPath, String outPath)
		{
#if BF_PLATFORM_WINDOWS
			if (!fontPath.Contains(':'))
			{
				char8[256] windowsDir;
				Windows.GetWindowsDirectoryA(&windowsDir, 256);
				outPath.Append(&windowsDir);
				outPath.Append(@"\Fonts\");
			}
#endif
			outPath.Append(fontPath);
		}

		static void AddFontNameKey(StringView key, StringView value)
		{
			if (key.IsEmpty)
				return;

			String upperKey = scope String(key);
			upperKey.ToUpper();
			if (sFontNameMap.TryAdd(upperKey, var keyPtr, var valuePtr))
			{
				*keyPtr = new String(upperKey);
				*valuePtr = new String(value);
			}
		}

		// Records one face for the name cache. FreeType supplies the face's real family and style so a lookup
		// matches by family the way a browser does, instead of requiring the OS's exact display name.
		static void AddFontFaceInfo(List<FontFaceInfo> faceInfos, StringView fontPath, int32 faceIndex, StringView displayName)
		{
			FontFaceInfo info = new FontFaceInfo();
			info.mPath = new String();
			ResolveFontPath(fontPath, info.mPath);
			if (faceIndex > 0)
				info.mPath.AppendF("@{}", faceIndex);
			if (!displayName.IsEmpty)
				info.mDisplayName = new String(displayName);

			char8[512] familyBuf;
			char8[512] styleBuf;
			int32 flags = 0;
			int32 weight = 400;
			int32 faceCount = FTFont_GetFaceInfo(info.mPath, faceIndex, &familyBuf, 512, &styleBuf, 512, &flags, &weight);
			if (faceCount > 0)
			{
				info.mFamily = new String(&familyBuf);
				info.mStyle = new String(&styleBuf);
				info.mRegular = (((flags & 1) == 0) && ((flags & 2) == 0) && (weight >= 350) && (weight <= 450));
			}
			else if (!displayName.IsEmpty)
			{
				// The file couldn't be opened; fall back to the OS display name as the family.
				info.mFamily = new String(displayName);
				info.mStyle = new String();
				info.mRegular = true;
			}
			else
			{
				delete info;
				return;
			}

			faceInfos.Add(info);
		}

		// Fills sFontNameMap with each face's family, "family style" and OS display name. A regular face claims
		// the bare family key first, so eg "Arial" resolves to Arial Regular rather than Arial Bold.
		static void PopulateFontNameMap(List<FontFaceInfo> faceInfos)
		{
			for (let face in faceInfos)
				if (face.mRegular)
					AddFontNameKey(face.mFamily, face.mPath);
			for (let face in faceInfos)
				AddFontNameKey(face.mFamily, face.mPath);
			for (let face in faceInfos)
				if ((face.mStyle != null) && (!face.mStyle.IsEmpty))
					AddFontNameKey(scope $"{face.mFamily} {face.mStyle}", face.mPath);
			for (let face in faceInfos)
				if (face.mDisplayName != null)
					AddFontNameKey(face.mDisplayName, face.mPath);
		}

		static void BuildFontNameCache()
		{
			using (sMonitor.Enter())
			{
				sFontNameMap = new .();

				var faceInfos = scope List<FontFaceInfo>();
				defer { ClearAndDeleteItems(faceInfos); }

#if BF_PLATFORM_WINDOWS

			for (int pass < 2)
			{
				Windows.HKey hkey;

				if (pass == 0)
				{
					if (Windows.RegOpenKeyExA(Windows.HKEY_LOCAL_MACHINE, @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts", 0,
						Windows.KEY_QUERY_VALUE | Windows.KEY_WOW64_32KEY | Windows.KEY_ENUMERATE_SUB_KEYS, out hkey) != Windows.S_OK)
						continue;
				}
				else
				{
					if (Windows.RegOpenKeyExA(Windows.HKEY_CURRENT_USER, @"SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts", 0,
						Windows.KEY_QUERY_VALUE | Windows.KEY_WOW64_32KEY | Windows.KEY_ENUMERATE_SUB_KEYS, out hkey) != Windows.S_OK)
						continue;
				}

				defer Windows.RegCloseKey(hkey);

				for (int32 i = 0; true; i++)
				{
					char16[256] fontNameArr;
					uint32 nameLen = 255;
					uint32 valType = 0;

					char16[256] data;
					uint32 dataLen = 256 * 2;
					int32 result = Windows.RegEnumValueW(hkey, i, &fontNameArr, &nameLen, null, &valType, &data, &dataLen);
					if (result == 0)
					{
						if (valType == 1)
						{
							String displayName = scope String(&fontNameArr);
							int parenPos = displayName.IndexOf(" (");
							if (parenPos != -1)
								displayName.RemoveToEnd(parenPos);
							String fontPath = scope String(&data);
							if ((!fontPath.EndsWith(".TTF", .OrdinalIgnoreCase)) && (!fontPath.EndsWith(".TTC", .OrdinalIgnoreCase)) &&
								(!fontPath.EndsWith(".OTF", .OrdinalIgnoreCase)))
								continue;

							if (displayName.Contains('&'))
							{
								// A TrueType collection: each "A & B" name is a face, in file order.
								int32 collectionIdx = 0;
								for (var namePart in displayName.Split('&', .RemoveEmptyEntries))
								{
									namePart.Trim();
									AddFontFaceInfo(faceInfos, fontPath, collectionIdx, namePart);
									collectionIdx++;
								}
							}
							else
							{
								AddFontFaceInfo(faceInfos, fontPath, 0, displayName);
							}
						}
					}
					else
					{
						if (result == Windows.ERROR_MORE_DATA)
							continue;

						break;
					}
				}
			}
#elif BF_PLATFORM_LINUX
				if (!IsFontconfigAvailable)
					return;

				if (FcInit() == 0)
					return;

				defer FcFini();

				let pattern = FcPatternCreate();
				defer FcPatternDestroy(pattern);
				let objectset = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_WEIGHT, FC_SLANT, FC_FILE, FC_INDEX, null);
				defer FcObjectSetDestroy(objectset);

				let fontSet = FcFontList(null, pattern, objectset);
				if (fontSet == null)
					return;
				defer FcFontSetDestroy(fontSet);

				for (let i < fontSet.nfont)
				{
					let font = fontSet.fonts[i];

					char8* pFile = null;
					if (FcPatternGetString(font, FC_FILE, 0, &pFile) != .Match)
						continue;

					char8* pFamily = null;
					if (FcPatternGetString(font, FC_FAMILY, 0, &pFamily) != .Match)
						continue;

					char8* pStyle = null;
					FcPatternGetString(font, FC_STYLE, 0, &pStyle);
					c_int weight = 0;
					bool hasWeight = FcPatternGetInteger(font, FC_WEIGHT, 0, &weight) == .Match;
					c_int slant = 0;
					bool hasSlant = FcPatternGetInteger(font, FC_SLANT, 0, &slant) == .Match;
					c_int index = 0;
					FcPatternGetInteger(font, FC_INDEX, 0, &index);

					FontFaceInfo info = new FontFaceInfo();
					info.mFamily = new String(StringView(pFamily));
					info.mStyle = (pStyle != null) ? new String(StringView(pStyle)) : new String();
					info.mPath = new String(StringView(pFile));
					if (index > 0)
						info.mPath.AppendF("@{}", index);
					// fontconfig weights: 80 = regular, 200 = bold; slant 0 = roman.
					info.mRegular = hasWeight && hasSlant && (weight == 80) && (slant == 0);
					faceInfos.Add(info);
				}
#endif

				PopulateFontNameMap(faceInfos);
			}
		}

		public static void ClearFontNameCache()
		{
			using (sMonitor.Enter())
			{
				DeleteDictionaryAndKeysAndValues!(sFontNameMap);
				sFontNameMap = null;
			}
		}

		public static void AddFontFailEntry(StringView mapFrom, StringView mapTo)
		{
			using (sMonitor.Enter())
			{
				if (sFontFailMap == null)
					sFontFailMap = new .();

				String str = new String(mapFrom);
				str.ToUpper();
				bool added = sFontFailMap.TryAdd(str, var keyPtr, var valuePtr);
				if (added)
				{
					*keyPtr = str;
					*valuePtr = new String(mapTo);
				}
				else
				{
					delete str;
					(*valuePtr).Set(mapTo);
				}
			}
		}

		public void Dispose(bool cacheRetain)
		{
			if (mEffectUsers != null)
			{
				for (var effect in mEffectUsers)
					effect.Forget(this);
				mEffectUsers.Clear();
			}

			if (mFTFont != null)
			{
				FTFont_Delete(mFTFont, cacheRetain);
				mFTFont = null;
			}

			if (mLowCharData != null)
			{
				for (var charData in mLowCharData)
					delete charData;
				DeleteAndNullify!(mLowCharData);
			}

			if (mCharData != null)
			{
				for (var charData in mCharData.Values)
					delete charData;
				DeleteAndNullify!(mCharData);
			}

			if (mAlternates != null)
			{
				for (var altFont in mAlternates)
					if (altFont.mOwned)
						delete altFont.mFont;
				DeleteAndNullify!(mAlternates);
			}

			DeleteAndNullify!(mPath);
			DeleteAndNullify!(mLoKerningTable);
		}

		public void Dispose()
		{
			Dispose(false);
		}

		public static void ClearCache()
		{
			FTFont_ClearCache();
		}

		public void AddAlternate(Font altFont)
		{
			AltFont altFontEntry;
			altFontEntry.mFont = altFont;
			altFontEntry.mOwned = false;
			mAlternates.Add(altFontEntry);
		}

		public Result<void> AddAlternate(String path, float pointSize = -1)
		{
			Font altFont = Try!(LoadFromFile(path, pointSize, mGlyphGuard));
			AltFont altFontEntry;
			altFontEntry.mFont = altFont;
			altFontEntry.mOwned = true;
			mAlternates.Add(altFontEntry);
			return .Ok;
		}

        public bool GetVal(String outVal, List<String> cmds, String find)
        {
            for (String cmd in cmds)
            {
                if (cmd.StartsWith(find) && (cmd[find.Length] == '='))
				{
                    outVal.Append(cmd, find.Length + 1);
					return true;
				}
            }

            return false;
        }

        public int32 GetValInt(List<String> cmds, String find)
        {
			String strVal = scope String();
			if (!GetVal(strVal, cmds, find))
				return 0;
            return int32.Parse(strVal);
        }

		public void CalcKerning()
		{
			for (char8 c0 = ' '; c0 < '\x80'; c0++)
			{
				for (char8 c1 = ' '; c1 < '\x80'; c1++)
				{
					float kernVal = FTFont_GetKerning(mFTFont, (int32)c0, (int32)c1);
					if (kernVal != 0)
					{
						if (mLoKerningTable == null)
							mLoKerningTable = new float[128*128];
						mLoKerningTable[(int32)c0 + ((int32)c1)*128] = kernVal;
					}
				}
			}
		}

		public float GetKerning(char32 c0, char32 c1)
		{
			if (mLoKerningTable == null)
				return 0;

			if ((c0 < '\x80') && (c1 < '\x80'))
			{
				return mLoKerningTable[(int32)c0 + ((int32)c1)*128];
			}

			return FTFont_GetKerning(mFTFont, (int32)c0, (int32)c1);
		}

		void GetFontPath(StringView fontName, String path)
		{
			if (fontName.StartsWith('['))
			{
				path.Set(fontName);
				if (FilePackManager.TryMakeMemoryString(path))
					return;
				path.Clear();
			}

			if (fontName.Contains('.'))
			{
				Path.GetAbsolutePath(fontName, BFApp.sApp.mInstallDir, path);
			}
			else
			{
				using (sMonitor.Enter())
				{
					if (sFontNameMap == null)
						BuildFontNameCache();

					let lookupStr = scope String(fontName)..ToUpper();
					String pathStr = null;
					if (!sFontNameMap.TryGetValue(lookupStr, out pathStr))
					{
						// Tolerate a redundant trailing style: "Foo Regular" should still find "Foo".
						if (lookupStr.EndsWith(" REGULAR", .Ordinal))
						{
							let trimmedStr = scope String(lookupStr);
							trimmedStr.RemoveFromEnd(" REGULAR".Length);
							sFontNameMap.TryGetValue(trimmedStr, out pathStr);
						}
					}

					if (pathStr != null)
					{
						ResolveFontPath(pathStr, path);
						return;
					}

					if ((sFontFailMap != null) && (sFontFailMap.TryGetValue(lookupStr, out pathStr)))
					{
						StringView lastPath = pathStr;
						for (let fontFailName in pathStr.Split('\0', .RemoveEmptyEntries))
						{
							let failName  = scope String(fontFailName)..ToUpper();
							if (sFontNameMap.TryGetValue(failName, out pathStr))
							{
								GetFontPath(fontFailName, path);
								return;
							}
							lastPath = fontFailName;
						}

						path.Append(lastPath);
						return;
					}
				}
			}
		}

        public bool Load(StringView fontName, float pointSize = -1, int32 glyphGuard = 0)
        {
			Dispose();
			mGlyphGuard = glyphGuard;
			mSubpixel = false;

			mCharData = new Dictionary<char32, CharData>();
			mLowCharData = new CharData[LOW_CHAR_COUNT];
			mAlternates = new List<AltFont>();

			float usePointSize = pointSize;
			mPath = new String();
			GetFontPath(fontName, mPath);

			if (mPath.IsEmpty)
				return false;

			String fontPath = scope String(mPath);
			if (pointSize == -1)
			{
				if (fontName.EndsWith(".fnt"))
				{
					mBitmapPages = new .();

					var contents = File.ReadAllText(fontName, .. scope .());
					contents.Replace("\r", "");
					for (var line in contents.Split('\n', .RemoveEmptyEntries))
					{
						bool CheckVal(StringView text, StringView key, ref int32 val)
						{
							if (!text.StartsWith(key))
								return false;
							if (text[key.Length] != '=')
								return false;
							val = int32.Parse(text.Substring(key.Length + 1));
							return true;
						}

						bool CheckVal(StringView text, String key, String val)
						{
							if (!text.StartsWith(key))
								return false;
							if (text[key.Length] != '=')
								return false;
							StringView sv = text.Substring(key.Length + 1);
							if (sv.StartsWith('"'))
								sv.RemoveFromStart(1);
							if (sv.EndsWith('"'))
								sv.RemoveFromEnd(1);
							val.Append(sv);
							return true;
						}

						if (line.StartsWith("page"))
						{
							String imageFileName = scope .();

							for (var text in line.Split(' ', .RemoveEmptyEntries))
								CheckVal(text, "file", imageFileName);

							var dir = Path.GetDirectoryPath(fontName, .. scope .());
							var imagePath = scope $"{dir}/{imageFileName}";

							BitmapPage page = new .();

							page.mImage = Image.LoadFromFile(imagePath);
							mBitmapPages.Add(page);
						}

						if (line.StartsWith("char "))
						{
							CharData charData = null;

							for (var text in line.Split(' ', .RemoveEmptyEntries))
							{
								int32 id = 0;
								if (CheckVal(text, "id", ref id))
								{
									charData = new .();
									if (id < LOW_CHAR_COUNT)
										mLowCharData[id] = charData;
									else
										mCharData[(.)id] = charData;
								}

								if (charData != null)
								{
									CheckVal(text, "x", ref charData.mX);
									CheckVal(text, "y", ref charData.mY);
									CheckVal(text, "width", ref charData.mWidth);
									CheckVal(text, "height", ref charData.mHeight);
									CheckVal(text, "xoffset", ref charData.mXOffset);
									CheckVal(text, "yoffset", ref charData.mYOffset);
									CheckVal(text, "xadvance", ref charData.mXAdvance);

									int32 pageIdx = -1;
									CheckVal(text, "page", ref pageIdx);

									if (pageIdx != -1)
										charData.mImageSegment = mBitmapPages[pageIdx].mImage.CreateImageSegment(charData.mX, charData.mY, charData.mWidth, charData.mHeight);
								}
							}
						}
					}
					return true;
				}

				fontPath.Set(BFApp.sApp.mInstallDir);
				fontPath.Append("fonts/SourceCodePro-Regular.ttf");
				usePointSize = 9;
			}
			mFTFont = FTFont_Load(fontPath, usePointSize);
			if (mFTFont == null)
				return false;

			mSubpixel = sSubpixel;
			CalcKerning();
            return true;
        }

        public static Result<Font> LoadFromFile(String path, float pointSize = -1, int32 glyphGuard = 0)
        {
			scope AutoBeefPerf("Font.LoadFromFile");

            Font font = new Font();
            if (!font.Load(path, pointSize, glyphGuard))
			{
				delete font;
                return .Err; //TODO: Make proper error
			}
            return font;
        }

        public bool HasChar(char32 theChar)
        {
			return true;
            /*if ((theChar >= (char8)0) && (theChar < (char8)LOW_CHAR_COUNT))
                return mLowCharData[(int)theChar] != null;
            return mCharData.ContainsKey(theChar);*/
        }

        public float GetWidth(StringView theString)
        {
            float curX = 0;

			int len = theString.Length;
			if (len == 0)
			{
				return 0;
			}

            char32 prevChar = (char8)0;
			for (var c in theString.DecodedChars)
			{
				int idx = @c.NextIndex;
				if (c == (char32)'\x01')
				{
					@c.NextIndex = idx + 4;
					continue;
				}
				else if (c == (char32)'\x02')
				{
					continue;
				}
                CharData charData = GetCharData((char32)c);
				if ((charData != null) && (!charData.mIsCombiningMark))
				{
	                curX += charData.mXAdvance;
	                if (prevChar != (char8)0)
	                {
	                    float kernAmount = GetKerning(prevChar, c);
	                    curX += kernAmount;
	                }
				}

                prevChar = c;

				if (idx >= len)
					break;
            }

            return curX;
        }

		static MarkPosition[] sMarkPositionsLow = new MarkPosition[0x70]
		//       0        1        2        3            4        5        6        7             8        9        A        B             C        D        E        F
       /*0*/(.AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .AboveC, .AboveC, .AboveC,
	   /*1*/ .AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .AboveE, .AboveC, .AboveC, /**/ .BelowC, .BelowC, .AboveE, .TopR,   /**/ .BelowC, .BelowC, .BelowC, .BelowC,
	   /*2*/ .BelowC, .BelowC, .BelowC, .BelowC, /**/ .BelowC, .BelowC, .BelowC, .BelowC, /**/ .BelowC, .BelowC, .BelowC, .BelowC, /**/ .BelowC, .BelowC, .BelowC, .BelowC,
       /*3*/ .BelowC, .BelowC, .BelowC, .BelowC, /**/ .OverC , .OverC , .OverC , .OverC , /**/ .OverC , .BelowC, .BelowC, .BelowC, /**/ .AboveC, .AboveC, .AboveC, .AboveC,
       /*4*/ .AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .BelowC, .AboveC, .BelowC, /**/ .BelowC, .BelowC, .AboveC, .AboveC, /**/ .AboveC, .BelowC, .BelowC, .OverC,
       /*5*/ .AboveC, .AboveC, .AboveC, .BelowC, /**/ .BelowC, .BelowC, .BelowC, .AboveC, /**/ .AboveE, .BelowC, .AboveC, .AboveC, /**/ .BelowR, .AboveR, .AboveR, .BelowR,
       /*6*/ .AboveR, .AboveR, .BelowR, .AboveC, /**/ .AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .AboveC, .AboveC, .AboveC, /**/ .AboveC, .AboveC, .AboveC, .AboveC,
			) ~ delete _;

		MarkPosition GetMarkPosition(char32 checkChar)
		{
			if ((checkChar >= '\u{0300}') && (checkChar <= '\u{036F}'))
			{
				return sMarkPositionsLow[(int)(checkChar - '\u{0300}')];
			}
			return .OverC;
		}

        CharData GetCharData(char32 checkChar)
        {
            CharData charData;
            if ((checkChar >= (char8)0) && (checkChar < (char32)LOW_CHAR_COUNT))
                charData = mLowCharData[(int)checkChar];
            else
                mCharData.TryGetValue(checkChar, out charData);
            if (charData == null)
			{
				for (int fontIdx = -1; fontIdx < mAlternates.Count; fontIdx++)
				{
					FTFont* ftFont;
					if (fontIdx == -1)
						ftFont = mFTFont;
					else
						ftFont = mAlternates[fontIdx].mFont.mFTFont;
					if (ftFont == null)
						continue;

					var ftGlyph = FTFont_AllocGlyphGuarded(ftFont, (int32)checkChar, fontIdx == mAlternates.Count - 1, mGlyphGuard);
					if (ftGlyph == null)
						continue;

					charData = new CharData();
					charData.mGuard = mGlyphGuard;
					charData.mX = ftGlyph.mX;
					charData.mY = ftGlyph.mY;
					charData.mWidth = ftGlyph.mWidth;
					charData.mHeight = ftGlyph.mHeight;
					charData.mXAdvance = ftGlyph.mXAdvance;
					charData.mXOffset = ftGlyph.mXOffset;
					charData.mYOffset = ftGlyph.mYOffset;
					charData.mImageSegment = new Image();
					charData.mImageSegment.mNativeTextureSegment = ftGlyph.mTextureSegment;
					charData.mImageSegment.mX = ftGlyph.mX;
					charData.mImageSegment.mY = ftGlyph.mY;
					charData.mImageSegment.mWidth = ftGlyph.mWidth;
					charData.mImageSegment.mHeight = ftGlyph.mHeight;
					charData.mImageSegment.mSrcWidth = ftGlyph.mWidth;
					charData.mImageSegment.mSrcHeight = ftGlyph.mHeight;

					if (mSubpixel)
					{
						// Same glyph rasterized with LCD/RGB antialiasing, into the same page. Drawn instead of
						// the grayscale image when the text is axis-aligned and unscaled (see Draw).
						var ftGlyphSubpixel = FTFont_AllocGlyphGuardedSubpixel(ftFont, (int32)checkChar, fontIdx == mAlternates.Count - 1, mGlyphGuard);
						if (ftGlyphSubpixel != null)
						{
							charData.mImageSegmentSubpixel = new Image();
							charData.mImageSegmentSubpixel.mNativeTextureSegment = ftGlyphSubpixel.mTextureSegment;
							charData.mImageSegmentSubpixel.mX = ftGlyphSubpixel.mX;
							charData.mImageSegmentSubpixel.mY = ftGlyphSubpixel.mY;
							charData.mImageSegmentSubpixel.mWidth = ftGlyphSubpixel.mWidth;
							charData.mImageSegmentSubpixel.mHeight = ftGlyphSubpixel.mHeight;
							charData.mImageSegmentSubpixel.mSrcWidth = ftGlyphSubpixel.mWidth;
							charData.mImageSegmentSubpixel.mSrcHeight = ftGlyphSubpixel.mHeight;
							charData.mXOffsetSubpixel = ftGlyphSubpixel.mXOffset;
							charData.mYOffsetSubpixel = ftGlyphSubpixel.mYOffset;
						}
						else
						{
							// This face can't produce LCD glyphs. Drop the font to grayscale so no draw ever
							// pairs a grayscale texel with the LCD shader. Already-cached subpixel images just
							// go unused.
							mSubpixel = false;
						}
					}

					charData.mIsCombiningMark = ((checkChar >= '\u{0300}') && (checkChar <= '\u{036F}')) || ((checkChar >= '\u{1DC0}') && (checkChar <= '\u{1DFF}'));
					if (charData.mIsCombiningMark)
						charData.mXAdvance = 0;
					if ((checkChar >= (char32)0) && (checkChar < (char32)LOW_CHAR_COUNT))
						mLowCharData[(int)checkChar] = charData;
					else
						mCharData[checkChar] = charData;
	                break;
				}
			}

			if (charData == null)
			{
				if (checkChar == (char32)'?')
					return null;
				return GetCharData((char32)'?');
			}

			if (mFontEffectStack?.IsEmpty == false)
			{
				var fontEffect = mFontEffectStack.Back;
				charData = fontEffect.Apply(this, charData);
			}

            return charData;
        }

		MarkRefData GetMarkRefData()
		{
			if (mMarkRefData == null)
			{
				mMarkRefData = new MarkRefData();
				var charData = GetCharData('o');
				mMarkRefData.mTop = charData.mYOffset;
				mMarkRefData.mBottom = charData.mYOffset + charData.mHeight;
			}
			return mMarkRefData;
		}

        public float GetWidth(char32 theChar)
        {
            CharData charData = GetCharData(theChar);
			if (charData == null)
				return 0;
            return charData.mXAdvance;
        }

        public int GetCharCountToLength(StringView theString, float maxLength, bool* hitTabStop = null)
        {
            float curX = 0;

			int startIdx = 0;
            char32 prevChar = (char8)0;
            for (var c in theString.DecodedChars)
            {
				int idx = @c.NextIndex;
				if (c == (char32)'\x01')
				{
					@c.NextIndex = idx + 4;
					//startIdx = idx;
					continue;
				}
				else if (c == (char32)'\x02')
				{
					//startIdx = idx;
					continue;
				}
				else if (c == (char32)'\t')
				{
					if (hitTabStop != null)
					{
						*hitTabStop = true;
						return startIdx;
					}
				}

                CharData charData = GetCharData(c);
                curX += charData.mXAdvance;
                if (prevChar != (char32)0)
                {
                    float kernAmount = GetKerning(prevChar, c);
					curX += kernAmount;
                }

                if (curX > maxLength)
                    return startIdx;

                prevChar = c;
				startIdx = idx;
            }

            return (int32)theString.Length;
        }

        public float GetLineSpacing()
        {
			if (mFTFont == null)
				return 0;
            return mFTFont.mHeight;
        }

		public float GetHeight()
		{
			if (mFTFont == null)
				return 0;
		    return mFTFont.mHeight;
		}

		public float GetAscent()
		{
			if (mFTFont == null)
				return 0;
		    return mFTFont.mAscent;
		}

		public float GetDescent()
		{
			if (mFTFont == null)
				return 0;
		    return mFTFont.mDescent;
		}

        public float GetWrapHeight(StringView theString, float width)
        {
            return Draw(null, theString, 0, 0, -1, width, FontOverflowMode.Wrap);
        }

		public static void StrEncodeColor(uint32 color, String outString)
		{
			uint32 colorVal = (color >> 1) & 0x7F7F7F7F;
			outString.Append('\x01');
			outString.Append((char8*)&colorVal, 4);
		}

		public static void StrEncodePopColor(String outString)
		{
			outString.Append('\x02');
		}

		public static char8[5] EncodeColor(uint32 color)
		{
			char8[5] val;
			val[0] = '\x01';
			*((uint32*)&val[1]) = (color >> 1) & 0x7F7F7F7F;
			return val;
		}

		public static char8 EncodePopColor()
		{
			return '\x02';
		}

		public static void StrRemoveColors(StringView theString, String str)
		{
			int len = theString.Length;
			if (len == 0)
			{
				return;
			}

			for (var c in theString.DecodedChars)
			{
				int idx = @c.NextIndex;
				if (c == (char32)'\x01')
				{
					@c.NextIndex = idx + 4;
					continue;
				}
				else if (c == (char32)'\x02')
				{
					continue;
				}
		        str.Append(c);

				if (idx >= len)
					break;
		    }
		}

		// Whether the graphics matrix leaves glyphs on the pixel grid with no scale or rotation -- the only
		// case where RGB subpixel coverage lines up with the display's subpixels.
		static bool IsSubpixelMatrix(Matrix matrix)
		{
			return (matrix.a == 1.0f) && (matrix.b == 0.0f) && (matrix.c == 0.0f) && (matrix.d == 1.0f);
		}

		// Per-mode glyph geometry. The grayscale and subpixel rasters of a glyph share an advance but not
		// necessarily their bitmap bearing/extent (LCD adds filter margin), so each draw uses the values that
		// match the image it actually draws.
		static int32 GetCharXOffset(CharData charData, bool subpixel)
		{
			if ((subpixel) && (charData.mImageSegmentSubpixel != null))
				return charData.mXOffsetSubpixel;
			return charData.mXOffset;
		}

		static int32 GetCharYOffset(CharData charData, bool subpixel)
		{
			if ((subpixel) && (charData.mImageSegmentSubpixel != null))
				return charData.mYOffsetSubpixel;
			return charData.mYOffset;
		}

		static int32 GetCharWidth(CharData charData, bool subpixel)
		{
			if ((subpixel) && (charData.mImageSegmentSubpixel != null))
				return (int32)charData.mImageSegmentSubpixel.mWidth;
			return charData.mWidth;
		}

		static int32 GetCharHeight(CharData charData, bool subpixel)
		{
			if ((subpixel) && (charData.mImageSegmentSubpixel != null))
				return (int32)charData.mImageSegmentSubpixel.mHeight;
			return charData.mHeight;
		}

        public void Draw(Graphics g, float x, float y, StringView theString, FontMetrics* fontMetrics = null)
        {
            float curX = x;
            float curY = y;

            Matrix newMatrix = Matrix();
			bool hasClipRect = g.mClipRect.HasValue;
            Rect clipRect = g.mClipRect.GetValueOrDefault();

			uint32 color = g.mColor;

			bool usingTextRenderState = mFontEffectStack?.IsEmpty != false;
			// Subpixel antialiasing assumes the glyph lands on whole pixels with the display's RGB stripes
			// horizontal, so it's only used when the graphics matrix is unscaled and unrotated. Effects
			// (outlines) render through the grayscale image too.
			bool useSubpixel = usingTextRenderState && mSubpixel && IsSubpixelMatrix(g.mMatrix);

			if (usingTextRenderState)
				g.PushTextRenderState(useSubpixel);

			float markTopOfs = 0;
			float markBotOfs = 0;

			CharData lastCharData = null;
			float lastCharX = 0;
            char32 prevChar = (char32)0;

			for (var c in theString.DecodedChars)
            {
				int idx = @c.NextIndex;

				if (c == (char32)'\x01') // Set new color
				{
					if (idx <= theString.Length - 4)
					{
						uint32 newColor = *(uint32*)(theString.Ptr + idx) << 1;
						color = Color.Mult(newColor, g.mColor);
						@c.NextIndex = idx + 4;
						continue;
					}
				}
				else if (c == (char32)'\x02') // Restore color
				{
					color = g.mColor;
					continue;
				}

                CharData charData = GetCharData(c);
				if (charData == null)
					continue;
				int32 charWidth = GetCharWidth(charData, useSubpixel);
				int32 charHeight = GetCharHeight(charData, useSubpixel);
				float drawX = curX + GetCharXOffset(charData, useSubpixel);
				float drawY = curY + GetCharYOffset(charData, useSubpixel);

				if ((charData.mIsCombiningMark) && (lastCharData != null))
				{
					var markRefData = GetMarkRefData();
					var markPos = GetMarkPosition(c);
					int32 lastXOffset = GetCharXOffset(lastCharData, useSubpixel);
					int32 lastYOffset = GetCharYOffset(lastCharData, useSubpixel);
					int32 lastWidth = GetCharWidth(lastCharData, useSubpixel);
					int32 lastHeight = GetCharHeight(lastCharData, useSubpixel);

					if (markPos == .TopR)
					{
						drawX = lastCharX + lastXOffset + lastWidth - charWidth / 2;
						drawY = curY + lastYOffset - charHeight / 2;
					}
					else if ((markPos == .AboveE) || (markPos == .OverE))
					{
						drawX = lastCharX + lastXOffset + lastWidth - charWidth / 2;
					}
					else
					{
						drawX = lastCharX + lastXOffset + (lastWidth / 2);
						if ((markPos == .AboveC) || (markPos == .BelowC) || (markPos == .OverC))
							drawX -= charWidth / 2;
					}

					if ((markPos == .AboveC) || (markPos == .AboveR))
					{
                        drawY += lastYOffset - markRefData.mTop - markTopOfs;
						markTopOfs += charHeight;
					}
					else if ((markPos == .BelowC) || (markPos == .BelowR))
					{
						drawY += (lastYOffset + lastHeight) - markRefData.mBottom + markBotOfs;
						markBotOfs += charHeight;
					}
				}
				else
				{
					lastCharX = curX;
					markTopOfs = 0;
					markBotOfs = 0;

					if (prevChar != (char8)0)
					{
					    float kernAmount = GetKerning(prevChar, c);
						curX += kernAmount;
						drawX += kernAmount;
					}
					lastCharData = charData;
				}

                newMatrix.SetMultiplied(drawX, drawY, ref g.mMatrix);
				bool isFullyClipped = false;
				if (hasClipRect)
				{
					if (newMatrix.tx < clipRect.mX - charWidth)
					{
						isFullyClipped = true;
					}
					else if (newMatrix.tx > clipRect.mX + clipRect.mWidth)
					{
						isFullyClipped = true;
						if ((newMatrix.a > 0) && (fontMetrics == null)) // Forward? If so, all future chars will clip
							break;
					}
				}

				Image charImage = charData.mImageSegment;
				if (useSubpixel && (charData.mImageSegmentSubpixel != null))
					charImage = charData.mImageSegmentSubpixel;

				if (!isFullyClipped)
                    charImage.Draw(newMatrix, g.ZDepth, color);

                curX += charData.mXAdvance;

                prevChar = c;
            }

			if (usingTextRenderState)
				g.PopRenderState();

			if (fontMetrics != null)
				fontMetrics.mMaxX = Math.Max(fontMetrics.mMaxX, curX);
        }

        public float Draw(Graphics g, StringView theString, float x, float y, int32 justification = -1, float width = 0, FontOverflowMode stringEndMode = FontOverflowMode.Overflow, FontMetrics* fontMetrics = null)
        {
            float drawHeight = 0;
			float useX = x;
			float useY = y;

			StringView workingStr = theString;
			String tempStr = null;

			void PopulateTempStr()
			{
				if (tempStr.Length != workingStr.Length)
				{
					tempStr.Clear();
					tempStr.Append(workingStr);
				}
			}

            while (true)
            {
                int32 crPos = (int32)workingStr.IndexOf('\n');
                if (crPos == -1)
                    break;

                float sectionHeight = Draw(g, StringView(workingStr, 0, crPos), useX, useY, justification, width, stringEndMode, fontMetrics);
                drawHeight += sectionHeight;
                useY += sectionHeight;
				workingStr = .(workingStr, crPos + 1, workingStr.Length - (crPos + 1));
            }

            if ((justification != -1) || (stringEndMode != FontOverflowMode.Overflow))
            {
                float aWidth;
                while (true)
                {
                    aWidth = GetWidth(workingStr);
                    if (aWidth <= width)
                        break;

                    if (stringEndMode == FontOverflowMode.Ellipsis)
                    {
                        float ellipsisLen = GetWidth(mEllipsis);
                        if (width < ellipsisLen)
                            return 0; // Don't draw at all if we don't even have enough space for the ellipsis

						int strLen = GetCharCountToLength(workingStr, width - ellipsisLen);
						tempStr = scope:: String(Math.Min(strLen, 128));
						tempStr.Append(theString, 0, strLen);
						tempStr.Append(mEllipsis);
						workingStr = tempStr;
                        aWidth = GetWidth(workingStr);
                        break;
                    }
                    else if (stringEndMode == FontOverflowMode.Truncate)
                    {
						int strLen = GetCharCountToLength(workingStr, width);
						tempStr = scope:: String(Math.Min(strLen, 128));
						tempStr.Append(theString, 0, strLen);
						workingStr = tempStr;
						aWidth = GetWidth(workingStr);
                        break;
                    }
                    else if (stringEndMode == FontOverflowMode.Wrap)
                    {
                        int32 maxChars = (int32)GetCharCountToLength(workingStr, width);
						if (maxChars == 0)
						{
							if (workingStr.IsEmpty)
								break;
							maxChars = 1;
						}

                        int32 checkIdx = maxChars;
						if (checkIdx < workingStr.Length)
						{
	                        while ((checkIdx > 0) && (!workingStr[checkIdx].IsWhiteSpace))
	                            checkIdx--;
						}

                        if (checkIdx == 0)
                        {
                            // Can't break on whitespace
                            checkIdx = maxChars;
                        }

                        if (checkIdx > 0)
                        {
                            if (fontMetrics != null)
                                fontMetrics.mLineCount++;

                            drawHeight += Draw(g, StringView(workingStr, 0, checkIdx), useX, useY, justification, width, .Truncate, fontMetrics);
                            useY += GetLineSpacing();
                            workingStr.Adjust(checkIdx);
                        }
                    }
                    else
                        break;
                }

                if (fontMetrics != null)
                    fontMetrics.mMaxWidth = Math.Max(fontMetrics.mMaxWidth, aWidth);

                if (justification == 0)
				{
					// This strange-looking construct is so that odd-length lines and even-length lines do not 'jitter'
					// relative to each other as we're resizing a window
                    useX += (((int)(width)&~1) - (int)aWidth) / 2;
				}
                else if (justification == 1)
                    useX += width - aWidth;
            }

            if (g != null)
            {
				Draw(g, useX, useY, workingStr, fontMetrics);
            }

            if (fontMetrics != null)
                fontMetrics.mMaxWidth = Math.Max(fontMetrics.mMaxWidth, GetWidth(workingStr));
            drawHeight += GetLineSpacing();

            if (fontMetrics != null)
                fontMetrics.mLineCount++;

            return drawHeight;
        }
    }
}
