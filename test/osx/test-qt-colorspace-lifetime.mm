#ifndef TEST_QT_INTERPOSE
#include <QtGui/QColorSpace>
#include <QtGui/QImage>
#endif
#include <CoreGraphics/CoreGraphics.h>
#include <cstdio>
#include <dlfcn.h>

#ifdef TEST_QT_INTERPOSE
static CGColorSpaceRef createdColorSpace;
static int colorSpaceOwners;
static int checkedImages;
extern "C" int qt_checked_images() { return checkedImages; }

static CGColorSpaceRef trackColorSpace(CFDataRef profile)
{
	createdColorSpace = CGColorSpaceCreateWithICCData(profile);
	colorSpaceOwners = 1;
	return createdColorSpace;
}

static CFTypeRef trackRetain(CFTypeRef object)
{
	if (object == createdColorSpace)
		++colorSpaceOwners;
	return CFRetain(object);
}

static void trackRelease(CFTypeRef object)
{
	if (object == createdColorSpace)
		--colorSpaceOwners;
	CFRelease(object);
}

static CGImageRef checkImage(size_t width, size_t height, size_t componentBits, size_t pixelBits, size_t stride,
			     CGColorSpaceRef colorSpace, CGBitmapInfo info, CGDataProviderRef provider,
			     const CGFloat *decode, bool interpolate, CGColorRenderingIntent intent)
{
	++checkedImages;
	// macOS may cache a released profile, hiding the use-after-free; check Qt's ownership before CoreGraphics uses it.
	if (colorSpace == createdColorSpace && colorSpaceOwners <= 0) {
		std::fputs("FAIL: Qt releases the image color space before CGImageCreate\n", stderr);
		return nullptr;
	}
	return CGImageCreate(width, height, componentBits, pixelBits, stride, colorSpace, info, provider, decode,
			     interpolate, intent);
}

__attribute__((used, section("__DATA,__interpose"))) static const struct {
	const void *replacement;
	const void *original;
} hooks[] = {
	{reinterpret_cast<const void *>(trackColorSpace), reinterpret_cast<const void *>(CGColorSpaceCreateWithICCData)},
	{reinterpret_cast<const void *>(trackRetain), reinterpret_cast<const void *>(CFRetain)},
	{reinterpret_cast<const void *>(trackRelease), reinterpret_cast<const void *>(CFRelease)},
	{reinterpret_cast<const void *>(checkImage), reinterpret_cast<const void *>(CGImageCreate)},
};
#else
int main()
{
	// A valid image profile exercises QTBUG-147602; untagged images hide the dangling pointer behind cached sRGB.
	for (int iteration = 0; iteration < 1000; ++iteration) {
		QImage image(32, 32, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::blue);
		image.setColorSpace(QColorSpace(QColorSpace::Primaries::SRgb, QColorSpace::TransferFunction::Gamma,
		                               1.5f + iteration / 2000.0f));
		CGImageRef converted = image.toCGImage();
		if (!converted || CGImageGetWidth(converted) != 32 ||
		    CGColorSpaceGetModel(CGImageGetColorSpace(converted)) != kCGColorSpaceModelRGB) {
			std::fputs("Qt image color-space conversion failed\n", stderr);
			return 1;
		}
		CGImageRelease(converted);
	}
	const auto checked = reinterpret_cast<int (*)()>(dlsym(RTLD_DEFAULT, "qt_checked_images"));
	if (!checked || checked() != 1000) {
		std::fputs("FAIL: color-space ownership instrumentation was not active\n", stderr);
		return 2;
	}
	std::puts("PASS: 1000 profiled QImage conversions retain their color space");
}
#endif
