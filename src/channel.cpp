/******************************************************************************\
 * Copyright (c) 2004-2006
 *
 * Author(s):
 *	Volker Fischer
 *
 ******************************************************************************
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later 
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT 
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more 
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
\******************************************************************************/

#include "channel.h"


/******************************************************************************\
* CChannelSet                                                                  *
\******************************************************************************/
int CChannelSet::GetFreeChan()
{
	/* look for a free channel */
	for (int i = 0; i < MAX_NUM_CHANNELS; i++)
	{
		if (!vecChannels[i].IsConnected())
			return i;
	}

	/* no free channel found, return invalid ID */
	return INVALID_CHANNEL_ID;
}

int CChannelSet::CheckAddr(const CHostAddress& Addr)
{
	CHostAddress InetAddr;

	/* Check for all possible channels if IP is already in use */
	for (int i = 0; i < MAX_NUM_CHANNELS; i++)
    {
		if (vecChannels[i].GetAddress(InetAddr))
		{
			/* IP found, return channel number */
			if (InetAddr == Addr)
				return i;
		}
	}

	/* IP not found, return invalid ID */
	return INVALID_CHANNEL_ID;
}

bool CChannelSet::PutData(const CVector<unsigned char>& vecbyRecBuf,
						  const int iNumBytesRead, const CHostAddress& HostAdr)
{
	Mutex.lock ();

	/* get channel ID ------------------------------------------------------- */
	bool bChanOK = true;

	/* check address */
	int iCurChanID = CheckAddr ( HostAdr );

	if ( iCurChanID == INVALID_CHANNEL_ID )
	{
		/* a new client is calling, look for free channel */
		iCurChanID = GetFreeChan ();

		if ( iCurChanID != INVALID_CHANNEL_ID )
		{
			vecChannels[iCurChanID].SetAddress ( HostAdr );
		}
		else
		{
			bChanOK = false; /* no free channel available */
		}
	}


	/* put received data in jitter buffer ----------------------------------- */
	if ( bChanOK )
	{
		/* put packet in socket buffer */
		if ( vecChannels[iCurChanID].PutData ( vecbyRecBuf, iNumBytesRead ) )
		{
			PostWinMessage ( MS_JIT_BUF_PUT, MUL_COL_LED_GREEN, iCurChanID );
		}
		else
		{
			PostWinMessage ( MS_JIT_BUF_PUT, MUL_COL_LED_RED, iCurChanID );
		}
	}

	Mutex.unlock ();

	return !bChanOK; /* return 1 if error */
}

void CChannelSet::GetBlockAllConC(CVector<int>& vecChanID,
								  CVector<CVector<double> >& vecvecdData)
{
	/* init temporal data vector and clear input buffers */
	CVector<double>	vecdData(BLOCK_SIZE_SAMPLES);

	vecChanID.Init(0);
	vecvecdData.Init(0);

	/* make put and get calls thread safe. Do not forget to unlock mutex
	   afterwards! */
	Mutex.lock();

	/* Check all possible channels */
	for (int i = 0; i < MAX_NUM_CHANNELS; i++)
	{
		/* read out all input buffers to decrease timeout counter on
		   disconnected channels */
		bool bGetOK = vecChannels[i].GetData(vecdData);

		if (vecChannels[i].IsConnected())
		{
			/* add ID and data */
			vecChanID.Add(i);

			const int iOldSize = vecvecdData.Size();
			vecvecdData.Enlarge(1);
			vecvecdData[iOldSize].Init(vecdData.Size());
			vecvecdData[iOldSize] = vecdData;

			/* send message for get status (for GUI) */
			if (bGetOK)
				PostWinMessage(MS_JIT_BUF_GET, MUL_COL_LED_GREEN, i);
			else
				PostWinMessage(MS_JIT_BUF_GET, MUL_COL_LED_RED, i);
		}
	}

	Mutex.unlock(); /* release mutex */
}

void CChannelSet::GetConCliParam(CVector<CHostAddress>& vecHostAddresses,
								 CVector<double>& vecdSamOffs)
{
	CHostAddress InetAddr;

	/* init return values */
	vecHostAddresses.Init(MAX_NUM_CHANNELS);
	vecdSamOffs.Init(MAX_NUM_CHANNELS);

	/* Check all possible channels */
	for (int i = 0; i < MAX_NUM_CHANNELS; i++)
	{
		if (vecChannels[i].GetAddress(InetAddr))
		{
			/* add new address and sample rate offset to vectors */
			vecHostAddresses[i] = InetAddr;
			vecdSamOffs[i] = vecChannels[i].GetResampleOffset();
		}
	}
}

void CChannelSet::SetSockBufSize ( const int iNewBlockSize, const int iNumBlocks )
{
	/* this opperation must be done with mutex */
	Mutex.lock ();

/* as a test we adjust the buffers of all channels to the new value. Maybe later
   do change only for some channels -> take care to set value back to default if
   channel is disconnected, afterwards! */
for ( int i = 0; i < MAX_NUM_CHANNELS; i++ )
	vecChannels[i].SetSockBufSize ( iNewBlockSize, iNumBlocks );

	Mutex.unlock ();
}


/******************************************************************************\
* CChannel                                                                     *
\******************************************************************************/
CChannel::CChannel()
{
	/* init time stamp activation counter */
	iTimeStampActCnt = NUM_BL_TIME_STAMPS;

	/* init time stamp index counter */
	byTimeStampIdxCnt = 0;

	/* init the socket buffer */
	SetSockBufSize ( BLOCK_SIZE_SAMPLES, DEF_NET_BUF_SIZE_NUM_BL );

	/* init conversion buffer */
	ConvBuf.Init ( BLOCK_SIZE_SAMPLES );

	/* init audio compression unit */
	iAudComprSize = AudioCompression.Init ( BLOCK_SIZE_SAMPLES,
		CAudioCompression::CT_IMAADPCM );

	/* init time-out for the buffer with zero -> no connection */
	iConTimeOut = 0;

	/* init sample rate offset estimation object */
	SampleOffsetEst.Init();
}

void CChannel::SetSockBufSize ( const int iNewBlockSize, const int iNumBlocks )
{
	/* this opperation must be done with mutex */
	Mutex.lock ();

	SockBuf.Init ( iNewBlockSize, iNumBlocks );

	Mutex.unlock ();
}

bool CChannel::GetAddress(CHostAddress& RetAddr)
{
	if (IsConnected())
	{
		RetAddr = InetAddr;
		return true;
	}
	else
	{
		RetAddr = CHostAddress();
		return false;
	}
}

bool CChannel::PutData(const CVector<unsigned char>& vecbyData,
					   int iNumBytes)
{
	bool bRet = true;

	Mutex.lock(); /* put mutex lock */

	/* only process if packet has correct size */
	if (iNumBytes == iAudComprSize)
	{
		/* decompress audio */
		CVector<short> vecsDecomprAudio(BLOCK_SIZE_SAMPLES);
		vecsDecomprAudio = AudioCompression.Decode(vecbyData);

		/* do resampling to compensate for sample rate offsets in the
		   different sound cards of the clients */
/*
for (int i = 0; i < BLOCK_SIZE_SAMPLES; i++)
	vecdResInData[i] = (double) vecsData[i];

const int iInSize = ResampleObj.Resample(vecdResInData, vecdResOutData,
	(double) SAMPLE_RATE / (SAMPLE_RATE - dSamRateOffset));
*/

vecdResOutData.Init(BLOCK_SIZE_SAMPLES);
for (int i = 0; i < BLOCK_SIZE_SAMPLES; i++)
	vecdResOutData[i] = (double) vecsDecomprAudio[i];


		bRet = SockBuf.Put(vecdResOutData);

		/* reset time-out counter */
		iConTimeOut = CON_TIME_OUT_CNT_MAX;
	}
	else if (iNumBytes == 1)
	{
		/* time stamp packet */
		SampleOffsetEst.AddTimeStampIdx(vecbyData[0]);
	}
	else
		bRet = false; /* wrong packet size */

	Mutex.unlock(); /* put mutex unlock */

	return bRet;
}

bool CChannel::GetData(CVector<double>& vecdData)
{
	Mutex.lock(); /* get mutex lock */

	const bool bGetOK = SockBuf.Get(vecdData);

	if (!bGetOK)
	{
		/* decrease time-out counter */
		if (iConTimeOut > 0)
		{
			iConTimeOut--;

			/* if time out is reached, re-init resample offset estimation
			   module */
			if (iConTimeOut == 0)
				SampleOffsetEst.Init();
		}
	}

	Mutex.unlock(); /* get mutex unlock */

	return bGetOK;
}

CVector<unsigned char> CChannel::PrepSendPacket(const CVector<short>& vecsNPacket)
{
	/* if the block is not ready we have to initialize with zero length to
	   tell the following network send routine that nothing should be sent */
	CVector<unsigned char> vecbySendBuf ( 0 );

	/* use conversion buffer to convert sound card block size in network
	   block size */
	if ( ConvBuf.Put ( vecsNPacket ) )
	{
		/* a packet is ready, compress audio */
		vecbySendBuf.Init ( iAudComprSize );
		vecbySendBuf = AudioCompression.Encode ( ConvBuf.Get () );
	}

	return vecbySendBuf;
}

int CChannel::GetTimeStampIdx ()
{
	/* only send time stamp index after a pre-defined number of packets */
	if ( iTimeStampActCnt > 0 )
	{
		iTimeStampActCnt--;
		return INVALID_TIME_STAMP_IDX;
	}
	else
	{
		/* reset time stamp activation counter */
		iTimeStampActCnt = NUM_BL_TIME_STAMPS - 1;

		/* wraps around automatically */
		byTimeStampIdxCnt++;
		return byTimeStampIdxCnt;
	}
}


/******************************************************************************\
* CSampleOffsetEst                                                             *
\******************************************************************************/
void CSampleOffsetEst::Init()
{
	/* init sample rate estimation */
	dSamRateEst = SAMPLE_RATE;

	/* init vectors storing the data */
	veciTimeElapsed.Init(VEC_LEN_SAM_OFFS_EST);
	veciTiStIdx.Init(VEC_LEN_SAM_OFFS_EST);

	/* start reference time (the counter wraps to zero 24 hours after the last
	   call to start() or restart, but this should not concern us since this
	   software will most probably not be used that long) */
	RefTime.start();

	/* init accumulated time stamp variable */
	iAccTiStVal = 0;

	/* init count (do not ship any result in init phase) */
	iInitCnt = VEC_LEN_SAM_OFFS_EST + 1;
}

void CSampleOffsetEst::AddTimeStampIdx(const int iTimeStampIdx)
{
	int i;

	const int iLastIdx = VEC_LEN_SAM_OFFS_EST - 1;

	/* take care of wrap of the time stamp index (byte wrap) */
	if (iTimeStampIdx < veciTiStIdx[iLastIdx] - iAccTiStVal)
		iAccTiStVal += _MAXBYTE + 1;

	/* add new data pair to the FIFO */
	for (i = 1; i < VEC_LEN_SAM_OFFS_EST; i++)
	{
		/* move old data */
		veciTimeElapsed[i - 1] = veciTimeElapsed[i];
		veciTiStIdx[i - 1] = veciTiStIdx[i];
	}

	/* add new data */
	veciTimeElapsed[iLastIdx] = RefTime.elapsed();
	veciTiStIdx[iLastIdx] = iAccTiStVal + iTimeStampIdx;

/*
static FILE* pFile = fopen("v.dat", "w");
for (i = 0; i < VEC_LEN_SAM_OFFS_EST; i++)
	fprintf(pFile, "%d\n", veciTimeElapsed[i]);
fflush(pFile);
*/


	/* calculate linear regression for sample rate estimation */
	/* first, calculate averages */
	double dTimeAv = 0;
	double dTiStAv = 0;
	for (i = 0; i < VEC_LEN_SAM_OFFS_EST; i++)
	{
		dTimeAv += veciTimeElapsed[i];
		dTiStAv += veciTiStIdx[i];
	}
	dTimeAv /= VEC_LEN_SAM_OFFS_EST;
	dTiStAv /= VEC_LEN_SAM_OFFS_EST;

	/* calculate gradient */
	double dNom = 0;
	double dDenom = 0;
	for (i = 0; i < VEC_LEN_SAM_OFFS_EST; i++)
	{
		const double dCurTimeNoAv = veciTimeElapsed[i] - dTimeAv;
		dNom += dCurTimeNoAv * (veciTiStIdx[i] - dTiStAv);
		dDenom += dCurTimeNoAv * dCurTimeNoAv;
	}

	/* final sample rate offset estimation calculation */
	if (iInitCnt > 0)
		iInitCnt--;
	else
	{
		dSamRateEst = dNom / dDenom * NUM_BL_TIME_STAMPS * BLOCK_SIZE_SAMPLES * 1000;

/*
static FILE* pFile = fopen("v.dat", "w");
for (i = 0; i < VEC_LEN_SAM_OFFS_EST; i++)
	fprintf(pFile, "%d %d\n", veciTimeElapsed[i], veciTiStIdx[i]);
fflush(pFile);
*/
	}

/*
static FILE* pFile = fopen("v.dat", "w");
fprintf(pFile, "%e\n", dSamRateEst);
fflush(pFile);
*/

}