/**
 * @file h264parser.h
 * @brief H264 parser class declaration
 *
 * Copyright: (c) 2025 by Andreas Baierl. All Rights Reserved.
 *
 * License: AGPLv3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 */

#ifndef __H264PARSER_H
#define __H264PARSER_H

extern "C" {
#include <libavcodec/avcodec.h>
}

/**
 * @brief cH264Parser - H264 Parser class
 */
class cH264Parser
{
public:
	cH264Parser(AVPacket *);
	virtual ~cH264Parser(void);
	void GetDimensions(int *, int *);
private:
	AVPacket *m_pAvpkt;
	const unsigned char *m_pStart;
	unsigned short m_nLength;
	int m_nCurrentBit;

	unsigned int ReadBit(void);
	unsigned int ReadBits(int);
	unsigned int ReadExponentialGolombCode(void);
	unsigned int ReadSE(void);
};

#endif
