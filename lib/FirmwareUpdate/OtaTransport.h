#pragma once

#include "FirmwareUpdate.h"

namespace FirmwareUpdate
{
struct Operation
{
	Operation(uint32_t requestGeneration, uint32_t start, uint32_t timeout)
		: generation(requestGeneration), started(start), budget(timeout) {}

	uint32_t generation;
	uint32_t started;
	uint32_t budget;
	Error error = Error::None;
	bool cancelled = false;
	int httpStatus = 0;

	bool checkpoint();
	uint32_t remaining() const;
	bool fail(Error reason);
};

class HttpsDownload
{
public:
	explicit HttpsDownload(Operation &operation);
	~HttpsDownload();
	HttpsDownload(const HttpsDownload &) = delete;
	HttpsDownload &operator=(const HttpsDownload &) = delete;

	bool open(const char *filename, const char *releaseTag, uint32_t maximumBytes);
	int read(uint8_t *buffer, size_t capacity);

private:
	struct Impl;
	Operation &operation_;
	Impl *impl_ = nullptr;
};
}
