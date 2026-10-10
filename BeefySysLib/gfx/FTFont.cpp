#include "FTFont.h"
#include "gfx/Texture.h"
#include "gfx/RenderDevice.h"
#include "img/ImageData.h"
#include "BFApp.h"

#include "freetype/ftsizes.h"

#include "util/AllocDebug.h"

USING_NS_BF;

const int FT_PAGE_WIDTH = 1024;
const int FT_PAGE_HEIGHT = 1024;

//const int FT_PAGE_WIDTH = 128;
//const int FT_PAGE_HEIGHT = 128;

static FT_Library gFTLibrary = NULL;
static FTFontManager gFTFontManager;

static void EnsureFTLibrary()
{
	if (gFTLibrary != NULL)
		return;

	FT_Init_FreeType(&gFTLibrary);
	// A 5-tap filter that blends neighbouring subpixels; without it LCD output shows colour fringing
	// on every edge rather than only where the coverage genuinely calls for it. Library-wide but only
	// affects LCD rasterization, so grayscale is untouched. Ignore failure: some builds omit the
	// optional subpixel-rendering config, in which case only the default filter is available.
	FT_Library_SetLcdFilter(gFTLibrary, FT_LCD_FILTER_DEFAULT);
}

FTFont::FTFont()
{
	mFace = NULL;
	mFaceSize = NULL;
	mHeight = 0;
	mAscent = 0;
	mDescent = 0;
	mMaxAdvance = 0;
}

FTFont::~FTFont()
{
	Dispose(false);
}

void FTFont::Dispose(bool cacheRetain)
{
	if (mFaceSize != NULL)
	{
		BF_ASSERT((mFaceSize->mRefCount > 0) && (mFaceSize->mRefCount < 1000000));
		mFaceSize->mRefCount--;

		if (mFaceSize->mRefCount == 0)
		{
			mFace->mFaceSizes.Remove(mFaceSize->mPointSize);
			delete mFaceSize;

			if (!cacheRetain)
			{
				if (mFace->mFaceSizes.IsEmpty())
				{
					bool removed = gFTFontManager.mFaces.Remove(mFace->mFileName);
					BF_ASSERT(removed);
					delete mFace;
				}
			}
		}
		mFaceSize = NULL;
	}
}

FTFontManager::FaceSize::~FaceSize()
{
	if (mFTSize != 0)
		FT_Done_Size(mFTSize);
}

FTFontManager::Face::~Face()
{
	if (mFTFace != 0)
		FT_Done_Face(mFTFace);
}

FTFontManager::FTFontManager()
{
	for (int i = 0; i < 256; i++)
	{
		uint8 whiteVal = (uint8)(pow(i / 255.0f, 0.75) * 255.0f);
		uint8 blackVal = (uint8)(pow(i / 255.0f, 0.9) * 255.0f);

		mWhiteTab[i] = whiteVal;
		mBlackTab[i] = blackVal;
	}
}

FTFontManager::~FTFontManager()
{
	for (auto page : mPages)
		delete page;
	mPages.Clear();
	for (auto faceKV : mFaces)
		delete faceKV.mValue;
	mFaces.Clear();
}

void FTFontManager::DoClearCache()
{
	for (auto page : mPages)
		delete page;
	mPages.Clear();

	for (auto itr = mFaces.begin(); itr != mFaces.end(); )
	{
		auto face = itr->mValue;
		if (face->mFaceSizes.IsEmpty())
		{
			delete face;
			itr = mFaces.Remove(itr);
		}
		else
			++itr;
	}
}

void FTFontManager::ClearCache()
{
	gFTFontManager.DoClearCache();
}

FTFontManager::Page::~Page()
{
	mTexture->Release();
	//delete mTexture;
}

/*FTFontManager::Glyph::~Glyph()
{
	//delete mTextureSegment;
}*/

bool FTFont::Load(const StringImpl& fileName, float pointSize)
{
	EnsureFTLibrary();

	FTFontManager::Face* face = NULL;

	String key = fileName;
	void* memPtr = NULL;
	int memLen = 0;
	bool isMemory = ParseMemorySpan(fileName, memPtr, memLen, &key);

	FTFontManager::Face** facePtr = NULL;
	if (gFTFontManager.mFaces.TryAdd(key, NULL, &facePtr))
	{
		face = new FTFontManager::Face();
		*facePtr = face;

		face->mFileName = key;
		FT_Face ftFace = NULL;

		String useFileName = fileName;
		int faceIdx = 0;
		int atPos = (int)useFileName.IndexOf('@', 1);
		if (atPos != -1)
		{
			faceIdx = atoi(useFileName.c_str() + atPos + 1);
			useFileName.RemoveToEnd(atPos);
		}
		
		if (isMemory)		
		{			
			FT_New_Memory_Face(gFTLibrary, (FT_Byte*)memPtr, memLen, faceIdx, &ftFace);
		}
		else
		{ 
			FT_New_Face(gFTLibrary, useFileName.c_str(), faceIdx, &ftFace);
		}
		
		face->mFTFace = ftFace;
	}
	else
	{
		face = *facePtr;
	}
	if (face->mFTFace == NULL)
		return false;

	mFace = face;

	FTFontManager::FaceSize** faceSizePtr = NULL;
	if (face->mFaceSizes.TryAdd(pointSize, NULL, &faceSizePtr))
	{
		//OutputDebugStrF("Created face %s %f\n", fileName.c_str(), pointSize);

		mFaceSize = new FTFontManager::FaceSize();
		*faceSizePtr = mFaceSize;

		FT_Size ftSize = NULL;
		FT_New_Size(mFace->mFTFace, &ftSize);
		FT_Activate_Size(ftSize);
		auto error = FT_Set_Char_Size(mFace->mFTFace, 0, (int)(pointSize * 64), 72, 72);

		mFaceSize->mFace = mFace;
		mFaceSize->mFTSize = ftSize;
		mFaceSize->mPointSize = pointSize;
	}
	else
	{
		mFaceSize = *faceSizePtr;
	}
	mFaceSize->mRefCount++;

	mHeight = mFaceSize->mFTSize->metrics.height / 64;
	mAscent = mFaceSize->mFTSize->metrics.ascender / 64;
	mDescent = mFaceSize->mFTSize->metrics.descender / 64;
	mMaxAdvance = mFaceSize->mFTSize->metrics.max_advance / 64;

	return true;
}

TextureSegment* BF_CALLTYPE Gfx_CreateTextureSegment(TextureSegment* textureSegment, int srcX, int srcY, int srcWidth, int srcHeight);

// guard: texels of empty glyph kept around the glyph in its page, so a filtered (scaled, rotated) draw of it
// never samples a neighbour or the page's unwritten fill.
// subpixel: see FTFont::AllocGlyph's declaration. The LCD bitmap is three subpixels wide per pixel; the
// three coverage channels are folded back into one texel so the glyph's extents (and advance) are unchanged
// from the grayscale path.
FTFontManager::Glyph* FTFont::AllocGlyph(int charCode, bool allowDefault, int guard, bool subpixel)
{
	FT_Activate_Size(mFaceSize->mFTSize);

	int glyph_index = FT_Get_Char_Index(mFace->mFTFace, charCode);
	if ((glyph_index == 0) && (!allowDefault))
		return NULL;

	auto error = FT_Load_Glyph(mFace->mFTFace, glyph_index, FT_LOAD_NO_BITMAP |
		(subpixel ? FT_LOAD_TARGET_LCD : FT_LOAD_TARGET_NORMAL));
	if (error != FT_Err_Ok)
		return NULL;

	error = FT_Render_Glyph(mFace->mFTFace->glyph, subpixel ? FT_RENDER_MODE_LCD : FT_RENDER_MODE_NORMAL);
	if (error != FT_Err_Ok)
		return NULL;

	auto& bitmap = mFace->mFTFace->glyph->bitmap;

	bool isLcd = subpixel && (bitmap.pixel_mode == FT_PIXEL_MODE_LCD);
	// FreeType can quietly fall back to a grayscale bitmap (eg a monochrome bitmap face). Packing that as
	// though it were subpixel would hand the LCD shader grayscale channels, so refuse instead -- the caller
	// then drops the font to grayscale. Empty glyphs are exempt: they have no channels and draw nothing.
	if (subpixel && !isLcd && (bitmap.width > 0) && (bitmap.rows > 0))
		return NULL;

	int pixelWidth = isLcd ? (int)(bitmap.width / 3) : (int)bitmap.width;

	// An empty glyph is never sampled.
	if ((pixelWidth == 0) || (bitmap.rows == 0))
		guard = 0;
	int cellWidth = pixelWidth + guard * 2;
	int cellHeight = (int)bitmap.rows + guard * 2;

	if ((cellHeight > FT_PAGE_HEIGHT) || (cellWidth > FT_PAGE_WIDTH))
	{
		return NULL;
	}

	FTFontManager::Page* page = NULL;

	if (!gFTFontManager.mPages.empty())
	{
		page = gFTFontManager.mPages.back();
		if (page->mCurX + cellWidth > page->mTexture->mWidth)
		{
			// Move down to next row
			page->mCurX = 0;
			page->mCurY += page->mMaxRowHeight;
			page->mMaxRowHeight = 0;
		}

		if (page->mCurY + cellHeight > page->mTexture->mHeight)
		{
			// Doesn't fit
			page = NULL;
		}
	}

	if (page == NULL)
	{
		page = new FTFontManager::Page();
		gFTFontManager.mPages.push_back(page);
	}

	//auto glyph = new FTFontManager::Glyph();

	static FTFontManager::Glyph staticGlyph;
	auto glyph = &staticGlyph;

	auto ftFace = mFace->mFTFace;

	glyph->mXAdvance = ftFace->glyph->advance.x / 64;
	glyph->mXOffset = ftFace->glyph->bitmap_left;
	glyph->mYOffset = ftFace->size->metrics.ascender / 64 - ftFace->glyph->bitmap_top;
	glyph->mPage = page;
	glyph->mX = page->mCurX + guard;
	glyph->mY = page->mCurY + guard;
	glyph->mWidth = pixelWidth;
	glyph->mHeight = bitmap.rows;

	if (page->mTexture == NULL)
	{
		ImageData* img = new ImageData();
		img->CreateNew(FT_PAGE_WIDTH, FT_PAGE_HEIGHT);
		auto* bits = img->mBits;
		for (int i = 0; i < FT_PAGE_HEIGHT * FT_PAGE_WIDTH; i++)
		{
			if (i % 3 == 0)
				bits[i] = 0xFFFF0000;
			else if (i % 3 == 1)
				bits[i] = 0xFF00FF00;
			else
				bits[i] = 0xFF0000FF;
		}
		page->mTexture = gBFApp->mRenderDevice->LoadTexture(img, TextureFlag_NoPremult);
		img->Deref();
	}

	if (pixelWidth > 0)
	{
		// Grayscale packs alpha=white-text coverage, red=black-text coverage (the text shader picks between
		// them by the text colour's luma); green/blue are ignored. Subpixel packs red/green/blue as the three
		// subpixel coverage channels -- the LCD text shader composes them per channel, so alpha is unused.
		auto GlyphTexel = [](uint8 val)
		{
			uint8 whiteVal = gFTFontManager.mWhiteTab[val];
			uint8 blackVal = gFTFontManager.mBlackTab[val];
			return (uint32)(((int32)whiteVal << 24) | ((int32)blackVal) | ((int32)0xFF << 8) | ((int32)0xFF << 16));
		};
		auto SubpixelTexel = [](uint8 r, uint8 g, uint8 b)
		{
			uint8 whiteR = gFTFontManager.mWhiteTab[r];
			uint8 whiteG = gFTFontManager.mWhiteTab[g];
			uint8 whiteB = gFTFontManager.mWhiteTab[b];
			return (uint32)(((int32)0xFF << 24) | ((int32)whiteB << 16) | ((int32)whiteG << 8) | ((int32)whiteR));
		};

		ImageData* img = new ImageData();
		img->CreateNew(cellWidth, cellHeight);
		auto* bits = img->mBits;
		int width = img->mWidth;
		uint32 empty = isLcd ? SubpixelTexel(0, 0, 0) : GlyphTexel(0);
		for (int i = 0; i < cellWidth * cellHeight; i++)
			bits[i] = empty;

		if (isLcd)
		{
			int pitch = (int)bitmap.pitch;
			for (int y = 0; y < (int)bitmap.rows; y++)
			{
				uint8* srcRow = bitmap.buffer + y * pitch;
				uint32* destRow = bits + (y + guard) * width + guard;
				for (int x = 0; x < pixelWidth; x++)
					destRow[x] = SubpixelTexel(srcRow[x * 3], srcRow[x * 3 + 1], srcRow[x * 3 + 2]);
			}
		}
		else
		{
			for (int y = 0; y < (int)bitmap.rows; y++)
			{
				for (int x = 0; x < (int)bitmap.width; x++)
					bits[(y + guard) * width + x + guard] = GlyphTexel(bitmap.buffer[y * bitmap.pitch + x]);
			}
		}

		page->mTexture->Blt(img, page->mCurX, page->mCurY);
		img->Deref();
	}

	page->mCurX += cellWidth;
	page->mMaxRowHeight = std::max(page->mMaxRowHeight, cellHeight);

	auto texture = page->mTexture;
	texture->AddRef();

	TextureSegment* textureSegment = new TextureSegment();
	textureSegment->mTexture = texture;
	textureSegment->mU1 = (glyph->mX / (float)texture->mWidth);
	textureSegment->mV1 = (glyph->mY / (float)texture->mHeight);
	textureSegment->mU2 = ((glyph->mX + glyph->mWidth) / (float)texture->mWidth);
	textureSegment->mV2 = ((glyph->mY + glyph->mHeight) / (float)texture->mHeight);
	textureSegment->mScaleX = (float)abs(glyph->mWidth);
	textureSegment->mScaleY = (float)abs(glyph->mHeight);

	glyph->mTextureSegment = textureSegment;
	return glyph;
}

int FTFont::GetKerning(int charA, int charB)
{
	FT_Activate_Size(mFaceSize->mFTSize);
	FT_Vector kerning;
	int glyph_indexA = FT_Get_Char_Index(mFace->mFTFace, charA);
	int glyph_indexB = FT_Get_Char_Index(mFace->mFTFace, charB);
	FT_Get_Kerning(mFace->mFTFace, glyph_indexA, glyph_indexB, FT_KERNING_DEFAULT, &kerning);
	return kerning.x / 64;
}

void FTFont::Release(bool cacheRetain)
{
	Dispose(cacheRetain);
	delete this;
}


//////////////////////////////////////////////////////////////////////////

BF_EXPORT FTFont* BF_CALLTYPE FTFont_Load(const char* fileName, float pointSize)
{
	auto ftFont = new FTFont();
	if (!ftFont->Load(fileName, pointSize))
	{
		delete ftFont;
		return NULL;
	}

	return ftFont;
}

BF_EXPORT void BF_CALLTYPE FTFont_ClearCache()
{
	FTFontManager::ClearCache();
}

BF_EXPORT void BF_CALLTYPE FTFont_Delete(FTFont* ftFont, bool cacheRetain)
{
	ftFont->Release(cacheRetain);
}

BF_EXPORT FTFontManager::Glyph* BF_CALLTYPE FTFont_AllocGlyph(FTFont* ftFont, int charCode, bool allowDefault)
{
	return ftFont->AllocGlyph(charCode, allowDefault);
}

BF_EXPORT FTFontManager::Glyph* BF_CALLTYPE FTFont_AllocGlyphGuarded(FTFont* ftFont, int charCode, bool allowDefault, int guard)
{
	return ftFont->AllocGlyph(charCode, allowDefault, guard);
}

BF_EXPORT FTFontManager::Glyph* BF_CALLTYPE FTFont_AllocGlyphSubpixel(FTFont* ftFont, int charCode, bool allowDefault)
{
	return ftFont->AllocGlyph(charCode, allowDefault, 0, true);
}

BF_EXPORT FTFontManager::Glyph* BF_CALLTYPE FTFont_AllocGlyphGuardedSubpixel(FTFont* ftFont, int charCode, bool allowDefault, int guard)
{
	return ftFont->AllocGlyph(charCode, allowDefault, guard, true);
}

BF_EXPORT int BF_CALLTYPE FTFont_GetKerning(FTFont* ftFont, int charCodeA, int charCodeB)
{
	auto kerning = ftFont->GetKerning(charCodeA, charCodeB);
	return kerning;
}

// Reads a font file's face names for the name cache, which needs the real family/style rather than the
// registry's display name in order to match names the way a browser does. Returns the number of faces in
// the file (>= 1), or 0 when the file couldn't be opened (missing, unsupported, bad faceIndex). faceIndex
// selects a face within a TrueType collection. Any output pointer may be NULL; outFlags carries bit0=bold,
// bit1=italic; outWeight is the OS/2 weight class (400 = normal). Buffers are null-terminated (empty when
// unavailable).
BF_EXPORT int BF_CALLTYPE FTFont_GetFaceInfo(const char* fileName, int faceIndex, char* outFamilyName, int familyNameBufSize,
	char* outStyleName, int styleNameBufSize, int* outFlags, int* outWeight)
{
	EnsureFTLibrary();

	if ((outFamilyName != NULL) && (familyNameBufSize > 0))
		outFamilyName[0] = 0;
	if ((outStyleName != NULL) && (styleNameBufSize > 0))
		outStyleName[0] = 0;
	if (outFlags != NULL)
		*outFlags = 0;
	if (outWeight != NULL)
		*outWeight = 400;

	FT_Face face = NULL;
	if (FT_New_Face(gFTLibrary, fileName, faceIndex, &face) != 0)
		return 0;

	int faceCount = face->num_faces;

	auto copyName = [](const char* src, char* dest, int destSize)
	{
		if ((dest == NULL) || (destSize <= 0))
			return;
		if (src == NULL)
		{
			dest[0] = 0;
			return;
		}
		int i = 0;
		while ((src[i] != 0) && (i < destSize - 1))
		{
			dest[i] = src[i];
			i++;
		}
		dest[i] = 0;
	};
	copyName(face->family_name, outFamilyName, familyNameBufSize);
	copyName(face->style_name, outStyleName, styleNameBufSize);

	if (outFlags != NULL)
	{
		int flags = 0;
		if ((face->style_flags & FT_STYLE_FLAG_BOLD) != 0)
			flags |= 1;
		if ((face->style_flags & FT_STYLE_FLAG_ITALIC) != 0)
			flags |= 2;
		*outFlags = flags;
	}

	if (outWeight != NULL)
	{
		int weight = 400;
		TT_OS2* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, ft_sfnt_os2);
		if ((os2 != NULL) && (os2->usWeightClass != 0))
			weight = os2->usWeightClass;
		*outWeight = weight;
	}

	FT_Done_Face(face);
	return faceCount;
}
