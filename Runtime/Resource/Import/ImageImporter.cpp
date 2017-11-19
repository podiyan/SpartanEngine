/*
Copyright(c) 2016-2017 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES =========================
#include "ImageImporter.h"
#include "FreeImagePlus.h"
#include <future>
#include <functional>
#include "../../Logging/Log.h"
#include "../../Core/Context.h"
#include "../../Threading/Threading.h"
#include "../../Graphics/Texture.h"
//====================================

//= NAMESPACES ================
using namespace std;
using namespace Directus::Math;
//=============================

namespace Directus
{
	ImageImporter::ImageImporter(Context* context)
	{
		m_context = context;
		FreeImage_Initialise(true);
	}

	ImageImporter::~ImageImporter()
	{
		FreeImage_DeInitialise();
	}

	void ImageImporter::LoadAsync(const string& filePath, TextureInfo& texInfo)
	{
		m_context->GetSubsystem<Threading>()->AddTask([this, &filePath, &texInfo]()
		{
			Load(filePath, texInfo);
		});
	}

	bool ImageImporter::Load(const string& filePath, TextureInfo& texInfo)
	{
		texInfo.loadState = Loading;

		// Validate the file path
		if (!ValidateFilePath(filePath))
		{
			texInfo.loadState = Failed;
			return false;
		}

		// In case this is an engine texture, load it directly
		if (FileSystem::IsEngineTextureFile(filePath))
		{
			if (!LoadEngineTexture(filePath, texInfo))
			{
				texInfo.loadState = Failed;
				return false;
			}

			texInfo.loadState = Completed;
			return true;
		}

		// Get image format
		FREE_IMAGE_FORMAT format = FreeImage_GetFileType(filePath.c_str(), 0);
		
		// If the format is unknown
		if (format == FIF_UNKNOWN)
		{
			// Try getting the format from the file extension
			LOG_WARNING("ImageImporter: Failed to determine image format for \"" + filePath + "\", attempting to detect it from the file's extension...");
			format = FreeImage_GetFIFFromFilename(filePath.c_str());

			// If the format is still unknown, give up
			if (!FreeImage_FIFSupportsReading(format))
			{
				LOG_WARNING("ImageImporter: Failed to detect the image format.");
				texInfo.loadState = Failed;
				return false;
			}

			LOG_WARNING("ImageImporter: The image format has been detected succesfully.");
		}

		// Get image format, format == -1 means the file was not found
		// but I am checking against it also, just in case.
		if (format == -1 || format == FIF_UNKNOWN)
		{
			texInfo.loadState = Failed;
			return false;
		}

		// Load the image as a FIBITMAP*
		FIBITMAP* bitmapOriginal = FreeImage_Load(format, filePath.c_str());

		// Flip it vertically
		FreeImage_FlipVertical(bitmapOriginal);

		// Perform any scaling (if necessary)
		bool userDefineDimensions = (texInfo.width != 0 && texInfo.height != 0);
		bool dimensionMismatch = (FreeImage_GetWidth(bitmapOriginal) != texInfo.width && FreeImage_GetHeight(bitmapOriginal) != texInfo.height);
		bool scale = userDefineDimensions && dimensionMismatch;
		FIBITMAP* bitmapScaled = scale ? FreeImage_Rescale(bitmapOriginal, texInfo.width, texInfo.height, FILTER_LANCZOS3) : bitmapOriginal;

		// Convert it to 32 bits (if neccessery)
		texInfo.bpp = FreeImage_GetBPP(bitmapOriginal);
		FIBITMAP* bitmap32 = texInfo.bpp != 32 ? FreeImage_ConvertTo32Bits(bitmapScaled) : bitmapScaled;
		texInfo.bpp = 32; // this is a hack, have to handle more elegant

		// Store some useful data	
		texInfo.isTransparent = bool(FreeImage_IsTransparent(bitmap32));
		texInfo.width = FreeImage_GetWidth(bitmap32);
		texInfo.height = FreeImage_GetHeight(bitmap32);
		texInfo.channels = ComputeChannelCount(bitmap32, texInfo.bpp);

		// Fill RGBA vector with the data from the FIBITMAP
		FIBTIMAPToRGBA(bitmap32, &texInfo.rgba);

		// Check if the image is grayscale
		texInfo.isGrayscale = GrayscaleCheck(texInfo.rgba, texInfo.width, texInfo.height);

		if (texInfo.isUsingMipmaps)
		{
			GenerateMipmapsFromFIBITMAP(bitmap32, texInfo);
		}

		//= Free memory =====================================
		// unload the 32-bit bitmap
		FreeImage_Unload(bitmap32);

		// unload the scaled bitmap only if it was converted
		if (texInfo.bpp != 32)
		{
			FreeImage_Unload(bitmapScaled);
		}

		// unload the non 32-bit bitmap only if it was scaled
		if (scale)
		{
			FreeImage_Unload(bitmapOriginal);
		}
		//====================================================

		texInfo.loadState = Completed;
		return true;
	}

	bool ImageImporter::ValidateFilePath(const string& filePath)
	{
		if (filePath.empty() || filePath == NOT_ASSIGNED)
		{
			LOG_WARNING("ImageImporter: Can't load image. No file path has been provided.");
			return false;
		}

		if (!FileSystem::FileExists(filePath))
		{
			LOG_WARNING("ImageImporter: Cant' load image. File path \"" + filePath + "\" is invalid.");
			return false;
		}

		return true;
	}

	bool ImageImporter::LoadEngineTexture(const string& filePath, TextureInfo& texInfo)
	{
		if (!texInfo.Deserialize(filePath))
		{
			LOG_WARNING("ImageImporter: Failed to load engine texture.");
			return false;
		}

		return true;
	}

	unsigned ImageImporter::ComputeChannelCount(FIBITMAP* fibtimap, unsigned int bpp)
	{
		FREE_IMAGE_TYPE imageType = FreeImage_GetImageType(fibtimap);
		if (imageType != FIT_BITMAP)
			return 0;

		if (bpp == 8)
			return 1;

		if (bpp == 24)
			return 3;

		if (bpp == 32)
			return 4;

		return 0;
	}

	bool ImageImporter::FIBTIMAPToRGBA(FIBITMAP* fibtimap, vector<unsigned char>* rgba)
	{
		int width = FreeImage_GetWidth(fibtimap);
		int height = FreeImage_GetHeight(fibtimap);

		unsigned int bytespp = width != 0 ? FreeImage_GetLine(fibtimap) / width : -1;
		if (bytespp == -1)
			return false;

		// Construct an RGBA array
		for (unsigned int y = 0; y < height; y++)
		{
			unsigned char* bits = (unsigned char*)FreeImage_GetScanLine(fibtimap, y);
			for (unsigned int x = 0; x < width; x++)
			{
				rgba->emplace_back(bits[FI_RGBA_RED]);
				rgba->emplace_back(bits[FI_RGBA_GREEN]);
				rgba->emplace_back(bits[FI_RGBA_BLUE]);
				rgba->emplace_back(bits[FI_RGBA_ALPHA]);

				// jump to next pixel
				bits += bytespp;
			}
		}

		return true;
	}

	void ImageImporter::GenerateMipmapsFromFIBITMAP(FIBITMAP* originalFIBITMAP, TextureInfo& texInfo)
	{
		// First mip is full size
		texInfo.rgba_mimaps.emplace_back(move(texInfo.rgba));
		int width = texInfo.width;
		int height = texInfo.height;

		// Compute the rest mip mipmapInfos
		struct scalingProcess
		{
			int width = 0;
			int height = 0;
			bool complete = false;
			vector<unsigned char> data;

			scalingProcess(int width, int height, bool scaled)
			{
				this->width = width;
				this->height = height;
				this->complete = scaled;
			}
		};
		vector<scalingProcess> scalingJobs;
		while (width > 1 && height > 1)
		{
			width = max(width / 2, 1);
			height = max(height / 2, 1);

			scalingJobs.emplace_back(width, height, false);
		}

		// Parallelize mipmap generation using multiple
		// threads as FreeImage_Rescale() using FILTER_LANCZOS3 can take a while.
		Threading* threading = m_context->GetSubsystem<Threading>();
		for (auto& job : scalingJobs)
		{
			threading->AddTask([this, &job, &texInfo, &originalFIBITMAP]()
			{
				if (!RescaleFIBITMAP(originalFIBITMAP, job.width, job.height, job.data))
				{
					string mipSize = "(" + to_string(job.width) + "x" + to_string(job.height) + ")";
					LOG_INFO("ImageImporter: Failed to create mip level " + mipSize + ".");
				}
				job.complete = true;
			});
		}

		// Wait until all mimaps have been generated
		bool ready = false;
		while (!ready)
		{
			ready = true;
			for (const auto& proccess : scalingJobs)
			{
				if (!proccess.complete)
				{
					ready = false;
				}
			}
		}

		// Now copy all the mimaps
		for (const auto& mimapInfo : scalingJobs)
		{
			texInfo.rgba_mimaps.emplace_back(move(mimapInfo.data));
		}
	}

	bool ImageImporter::RescaleFIBITMAP(FIBITMAP* fibtimap, int width, int height, vector<unsigned char>& rgba)
	{
		// Rescale
		FIBITMAP* scaled = FreeImage_Rescale(fibtimap, width, height, FILTER_LANCZOS3);

		// Extract RGBA data from the FIBITMAP
		bool result = FIBTIMAPToRGBA(scaled, &rgba);

		// Unload the FIBITMAP
		FreeImage_Unload(scaled);

		return result;
	}

	bool ImageImporter::GrayscaleCheck(const vector<unsigned char>& dataRGBA, int width, int height)
	{
		int grayPixels = 0;
		int totalPixels = width * height;
		int channels = 4;

		for (int i = 0; i < height; i++)
		{
			for (int j = 0; j < width; j++)
			{
				int red = dataRGBA[(i * width + j) * channels + 0];
				int green = dataRGBA[(i * width + j) * channels + 1];
				int blue = dataRGBA[(i * width + j) * channels + 2];

				if (red == green && red == blue)
				{
					grayPixels++;
				}
			}
		}

		return grayPixels == totalPixels;
	}
}
