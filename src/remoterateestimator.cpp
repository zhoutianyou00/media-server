/* 
 * File:   remoterateestimator.cpp
 * Author: Sergio
 * 
 * Created on 8 de marzo de 2013, 10:43
 */

#include <map>

#include "remoterateestimator.h"

RemoteRateEstimator::RemoteRateEstimator() : bitrateAcu(500)
{
	//Not last estimate
	minConfiguredBitRate	= 30000,
	maxConfiguredBitRate	= 30000000,
	currentBitRate		= maxConfiguredBitRate,
	maxHoldRate		= 0,
	avgMaxBitRate		= -1.0f,
	varMaxBitRate		= 0.4f,
	lastBitRateChange	= -1,
	avgChangePeriod		= 1000.0f,
	lastChangeMs		= -1,
	beta			= 0.9f,
	rtt			=  200;
	//Set initial state and region
	state = Decrease;
	state = Hold;
	region = MaxUnknown;

}
void RemoteRateEstimator::AddStream(DWORD ssrc,RemoteRateControl* ctrl)
{
	//Add it
	streams[ssrc] = ctrl;
}
void RemoteRateEstimator::RemoveStream(DWORD ssrc)
{
	//Remove
	streams.erase(ssrc);
}

void RemoteRateEstimator::Update(DWORD size)
{
	//Lock
	lock.WaitUnusedAndLock();

	//Get now
	QWORD now = getTime()/1000;

	//Acumulate
	bitrateAcu.Update(size,now);

	//Get global usage for all streams
	RemoteRateControl::BandwidthUsage usage = RemoteRateControl::UnderUsing;

	//For each one
	for (Streams::iterator it = streams.begin(); it!=streams.end(); ++it)
	{
		//Get stream usage
		RemoteRateControl::BandwidthUsage streamUsage = it->second->GetUsage();
		//Get worst
		if (usage<streamUsage)
			//Set it
			usage = streamUsage;
	}

	//No noise var calculationyet
	DWORD noiseVar = 1;
	
	// Get current bitrate
	const float incomingBitRate = bitrateAcu.GetInstantAvg();
	// Calculate the max bit rate std dev given the normalized
	// variance and the current incoming bit rate.
	const float stdMaxBitRate = sqrt(varMaxBitRate * avgMaxBitRate);

	bool recovery = false;

	//Depending on curren state
	switch (state)
	{
		case Hold:
			maxHoldRate = fmax(maxHoldRate, incomingBitRate);
			break;
		case Increase:
		{
			if (avgMaxBitRate >= 0)
			{
				if (incomingBitRate > avgMaxBitRate + 3 * stdMaxBitRate)
				{
					ChangeRegion(MaxUnknown);
					avgMaxBitRate = -1.0;
				} else if (incomingBitRate > avgMaxBitRate + 2.5 * stdMaxBitRate)
				{
					ChangeRegion(AboveMax);
				}
			}

			Log("BWE: Response time: %f + %i + 10*33\n", avgChangePeriod, rtt);

			const DWORD responseTime = (DWORD) (avgChangePeriod + 0.5f) + rtt + 300;
			double alpha = RateIncreaseFactor(now, lastBitRateChange, responseTime, noiseVar);

			Log("BWE: avgChangePeriod = %f ms; RTT = %u ms", avgChangePeriod, rtt);

			currentBitRate = (DWORD) (currentBitRate * alpha) + 1000;

			if (maxHoldRate > 0 && beta * maxHoldRate > currentBitRate)
			{
				currentBitRate = (DWORD) (beta * maxHoldRate);
				avgMaxBitRate = beta * maxHoldRate / 1000.0f;
				ChangeRegion(NearMax);
				recovery = true;
			}

			maxHoldRate = 0;
			Log("BWE: Increase rate to currentBitRate = %u kbps", currentBitRate / 1000);
			lastBitRateChange = now;
			break;
		}
		case Decrease:
			if (incomingBitRate < minConfiguredBitRate)
			{
				currentBitRate = minConfiguredBitRate;
			} else	{
				// Set bit rate to something slightly lower than max
				// to get rid of any self-induced delay.
				currentBitRate = (DWORD) (beta * incomingBitRate + 0.5);
				if (currentBitRate > currentBitRate)
				{
					// Avoid increasing the rate when over-using.
					if (region != MaxUnknown)
					{
						currentBitRate = (DWORD) (beta * avgMaxBitRate * 1000 + 0.5f);
					}
					currentBitRate = fmin(currentBitRate, currentBitRate);
				}

				ChangeRegion(NearMax);

				if (incomingBitRate < avgMaxBitRate - 3 * stdMaxBitRate)
				{
					avgMaxBitRate = -1.0f;
				}

				UpdateMaxBitRateEstimate(incomingBitRate);

				Log("BWE: Decrease rate to currentBitRate = %u kbps", currentBitRate / 1000);
			}
			// Stay on hold until the pipes are cleared.
			ChangeState(Hold);
			lastBitRateChange = now;
			break;
	}
	
	if (!recovery && (incomingBitRate > 100000 || currentBitRate > 150000) && currentBitRate > 1.5 * incomingBitRate)
	{
		// Allow changing the bit rate if we are operating at very low rates
		// Don't change the bit rate if the send side is too far off
		currentBitRate = currentBitRate;
		lastBitRateChange = now;
	}

	//Unlock
	lock.Unlock();
}

double RemoteRateEstimator::RateIncreaseFactor(QWORD nowMs, QWORD lastMs, DWORD reactionTimeMs, double noiseVar) const
{
	// alpha = 1.02 + B ./ (1 + exp(b*(tr - (c1*s2 + c2))))
	// Parameters
	const double B = 0.0407;
	const double b = 0.0025;
	const double c1 = -6700.0 / (33 * 33);
	const double c2 = 800.0;
	const double d = 0.85;

	double alpha = 1.005 + B / (1 + exp(b * (d * reactionTimeMs - (c1 * noiseVar + c2))));

	if (alpha < 1.005)
		alpha = 1.005;
	else if (alpha > 1.3)
		alpha = 1.3;


	if (lastMs > -1)
		alpha = pow(alpha, (nowMs - lastMs) / 1000.0);

	if (region == NearMax)
		// We're close to our previous maximum. Try to stabilize the
		// bit rate in this region, by increasing in smaller steps.
		alpha = alpha - (alpha - 1.0) / 2.0;
	else if (region == MaxUnknown)
		alpha = alpha + (alpha - 1.0) * 2.0;

	return alpha;
}

void RemoteRateEstimator::UpdateChangePeriod(QWORD nowMs)
{
	QWORD changePeriod = 0;
	if (lastChangeMs > -1)
		changePeriod = nowMs - lastChangeMs;
	lastChangeMs = nowMs;
	avgChangePeriod = 0.9f * avgChangePeriod + 0.1f * changePeriod;
}

void RemoteRateEstimator::UpdateMaxBitRateEstimate(float incomingBitRate)
{
	const float alpha = 0.05f;
	
	if (avgMaxBitRate == -1.0f)
		avgMaxBitRate = incomingBitRate;
	else
		avgMaxBitRate = (1 - alpha) * avgMaxBitRate + alpha * incomingBitRate;

	// Estimate the max bit rate variance and normalize the variance with the average max bit rate.
	const float norm = fmax(avgMaxBitRate, 1.0f);

	varMaxBitRate = (1 - alpha) * varMaxBitRate + alpha * (avgMaxBitRate - incomingBitRate) * (avgMaxBitRate - incomingBitRate) / norm;

	// 0.4 ~= 14 kbit/s at 500 kbit/s
	if (varMaxBitRate < 0.4f)
		varMaxBitRate = 0.4f;

	// 2.5f ~= 35 kbit/s at 500 kbit/s
	if (varMaxBitRate > 2.5f)
		varMaxBitRate = 2.5f;
}

DWORD RemoteRateEstimator::GetEstimatedBitrate()
{
	//Retun estimation
	return bitrateAcu.IsInWindow() ? currentBitRate : 0;
}

void RemoteRateEstimator::GetSSRCs(std::list<DWORD> &ssrcs)
{
	//For each one
	for (Streams::iterator it = streams.begin();  it!=streams.end(); ++it)
		//add ssrc
		ssrcs.push_back(it->first);
}
void RemoteRateEstimator::ChangeState(State newState)
{
	Log("-ChangeState from:%s to %s\n",GetName(state),GetName(newState));
	//Store values
	cameFromState = state;
	state = newState;
}

void RemoteRateEstimator::ChangeRegion(Region newRegion)
{
	//Store new region
	region = newRegion;
	//Calculate new beta
	switch (region)
	{
		case AboveMax:
		case MaxUnknown:
			beta = 0.9f;
			break;
		case NearMax:
			beta = 0.95f;
			break;
	}
}

void RemoteRateEstimator::SetRTT(DWORD rtt)
{
	//Update
	this->rtt = rtt;
}