//===============================================================================
// Copyright (c) 2018, Optimal Alpha LLC
//===============================================================================

#include <exception>
#include <iostream>
#include "CAlgorithm.h"
#include "CEvent.h"
#include "CConsolidatedBook.h"
#include "CQuote.h"
#include "CTrade.h"
#include "COrderStatus.h"
#include "COrderFill.h"
#include "CConfig.h"
#include "CATSException.h"
#include "CApplicationLog.h"
#include "CRecursiveLock.h"
#include "CScopedLock.h"


using namespace Ats::Algorithm;


CAlgorithm::CAlgorithm(unsigned int					uiAlgoID,
											 const std::string &	strAlgoName,
											 unsigned int					uiEventBufferSizeBytes,
											 unsigned int					uiUpdateMSec)
: m_uiAlgoID(uiAlgoID),
	m_strAlgoName(strAlgoName),

#ifdef LOCKFREE
	m_EventIDQ(uiEventBufferSizeBytes),
	m_EventOrderBookQ(uiEventBufferSizeBytes),
	m_EventBidAskQ(uiEventBufferSizeBytes),
	m_EventQuoteQ(uiEventBufferSizeBytes),
	m_EventTradeQ(uiEventBufferSizeBytes),
	m_EventOrderStatusQ(uiEventBufferSizeBytes),
	m_EventOrderFillQ(uiEventBufferSizeBytes),
	m_EventDateTimeQ(uiEventBufferSizeBytes),
#else
	m_EventQ(uiEventBufferSizeBytes),
#endif

	m_ptrThread(),
	m_iOperationalMode(PASSIVE),
	m_iUpdateMSec(uiUpdateMSec),
	m_iUpdateMSecPnL(5000),
	m_dMinTradeIntervalDayFrac(0),
	m_bEventQEnabled(true),
	m_dDailyMaxLoss(0.0),
	m_dCurrPnLUSD(0.0),
	m_dMaxPnLUSD(0.0),
	m_dStartOfDayPnLUSD(0.0),
	m_bPortfolioStopHit(false),
	m_DrawdownControl() {

	m_dUpdateIntervalDayFrac					= static_cast<double>(m_iUpdateMSec) / (24.0 * 60.0 * 60.0 * 1000.0);
	m_dDefaultUpdateIntervalDayFrac		= m_dUpdateIntervalDayFrac;
	m_dThrottledUpdateIntervalDayFrac	= static_cast<double>(10.0) / (24.0 * 60.0 * 60.0 * 1000.0);

	memset(m_TradedInstrumentMap, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(unsigned int));
	memset(m_iMaxPositionBaseCcy, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(int));
	memset(m_iMaxOrderSizeBaseCcy, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(int));
	memset(m_dCurrPairPnLUSD, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(double));
	memset(m_dMaxPairPnLUSD, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(double));
	memset(m_dStartOfDayPairPnLUSD, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(double));
	memset(m_dCurrCcyPnLUSD, 0, Ats::Core::CConfig::MAX_NUM_CCYS * sizeof(double));
	memset(m_dMaxCcyPnLUSD, 0, Ats::Core::CConfig::MAX_NUM_CCYS * sizeof(double));
	memset(m_dStartOfDayCcyPnLUSD, 0, Ats::Core::CConfig::MAX_NUM_CCYS * sizeof(double));

	memset(m_dLastExecPx, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(double));
	memset(m_dLastExecDirection, 0, Ats::Core::CConfig::MAX_NUM_SYMBOLS * sizeof(double));

	for (unsigned int i = 0; i != Ats::Core::CConfig::MAX_NUM_SYMBOLS; ++i) {
	
		m_RollingSpreadHistFast[i]	= Ats::Indicator::CCircularScalarBuffer(10, 1);
		m_RollingSpreadHistSlow[i]	= Ats::Indicator::CCircularScalarBuffer(60, 1);
		m_dtLastExecTime[i]					= 0;

		for (unsigned int j = 0; j != DAYS_PER_WEEK * MINUTES_PER_DAY; ++j) {

			m_SpreadMAHist[i][j] = 0;
		}

		// rolling ATR over past 60 seconds
		m_RollingATR[i] = Ats::Indicator::CRollingATR(12,5);
	}

	// reval intervals in seconds
	m_RevalIntSecsArray.push_back(0.1);
	m_RevalIntSecsArray.push_back(0.5);
	m_RevalIntSecsArray.push_back(1.0);
	m_RevalIntSecsArray.push_back(5.0);
	m_RevalIntSecsArray.push_back(10.0);
	m_RevalIntSecsArray.push_back(30.0);

	for (size_t i = 0; i != m_RevalIntSecsArray.size(); ++i) {

		m_RevalIntDayFracArray.push_back(m_RevalIntSecsArray[i] / (24.0 * 60.0 * 60.0));
		m_Revals[i] = std::vector<double>(Ats::Core::CConfig::MAX_NUM_SYMBOLS, 0.0);
	}
}


void CAlgorithm::run() {

	m_ptrThread = boost::shared_ptr<boost::thread>(new boost::thread(boost::ref(*this)));
}


#ifdef LOCKFREE

void CAlgorithm::operator()() {

	Ats::Log::CApplicationLog::getInstance().log("Algorithm started");

	try {

		while (isStopped() == false) {

			Ats::Event::CEvent::EventID iEventID;

			bool bRet = m_EventIDQ.pop(iEventID);

			if (bRet == false)
				continue;

			if (iEventID == Ats::Event::CEvent::UNDEFINED) {

				throw Ats::Exception::CATSException("undefined event read from queue");
			}

			if (iEventID == Ats::Event::CEvent::CONSOLIDATED_ORDER_BOOK) {

				Ats::OrderBook::CConsolidatedBook objOrderBook;

				bRet = m_EventOrderBookQ.pop(objOrderBook);

				if (bRet == true)
					onEvent(objOrderBook);
			}

			if (iEventID == Ats::Event::CEvent::BID_ASK) {

				Ats::OrderBook::CBidAsk objBidAsk;

				bRet = m_EventBidAskQ.pop(objBidAsk);

				if (bRet == true)
					onEvent(objBidAsk);
			}

			if (iEventID == Ats::Event::CEvent::QUOTE) {

				Ats::OrderBook::CQuote objQuote;

				bRet = m_EventQuoteQ.pop(objQuote);

				if (bRet == true)
					onEvent(objQuote);
			}

			if (iEventID == Ats::Event::CEvent::MKT_TRADE) {

				Ats::OrderBook::CTrade objTrade;

				bRet = m_EventTradeQ.pop(objTrade);

				if (bRet == true)
					onEvent(objTrade);
			}

			if (iEventID == Ats::Event::CEvent::ORDER_STATUS) {

				Ats::Order::COrderStatus objOrderStatus;

				bRet = m_EventOrderStatusQ.pop(objOrderStatus);

				if (bRet == true)
					onEvent(objOrderStatus);
			}

			if (iEventID == Ats::Event::CEvent::FILL) {

				Ats::Order::COrderFill objFill;

				bRet = m_EventOrderFillQ.pop(objFill);

				if (bRet == true)
					onEvent(objFill);
			}

			if (iEventID == Ats::Event::CEvent::DATETIME) {

				Ats::Core::CDateTimeEvent objDateTime;

				bRet = m_EventDateTimeQ.pop(objDateTime);

				if (bRet == true)
					onEvent(objDateTime);
			}
		}
	}
	catch(std::exception & e) {

		Ats::Log::CApplicationLog::getInstance().log(e.what());
	}
}

#else

void CAlgorithm::operator()() {

	Ats::Log::CApplicationLog::getInstance().log("Algorithm started");

	try {

		while (isStopped() == false) {

			m_EventQ.lock();

			while (m_EventQ.isReadReady() == false) {
				m_EventQ.wait();
			}

			Ats::Event::CEvent::EventID iEventID = m_EventQ.getNextEventID();

			switch (iEventID) {

				case Ats::Event::CEvent::UNDEFINED:
					m_EventQ.unlock();
					throw Ats::Exception::CATSException("undefined event read from queue");

				case Ats::Event::CEvent::CONSOLIDATED_ORDER_BOOK:
					handleEvent<Ats::OrderBook::CConsolidatedBook>(iEventID);
					break;

				case Ats::Event::CEvent::BID_ASK:
					handleEvent<Ats::OrderBook::CBidAsk>(iEventID);
					break;

				case Ats::Event::CEvent::QUOTE:
					handleEvent<Ats::OrderBook::CQuote>(iEventID);
					break;

				case Ats::Event::CEvent::MKT_TRADE:
					handleEvent<Ats::OrderBook::CTrade>(iEventID);
					break;

				case Ats::Event::CEvent::ORDER_STATUS:
					handleEvent<Ats::Order::COrderStatus>(iEventID);
					break;

				case Ats::Event::CEvent::FILL:
					handleEvent<Ats::Order::COrderFill>(iEventID);
					break;

				case Ats::Event::CEvent::EXEC_REPORT:
					handleEvent<Ats::Order::CExecReport>(iEventID);
					break;

				case Ats::Event::CEvent::DATETIME:
					handleEvent<Ats::Core::CDateTimeEvent>(iEventID);
					break;
			}
		}
	}
	catch(std::exception & e) {

		Ats::Log::CApplicationLog::getInstance().log(e.what());
	}
}

#endif


bool CAlgorithm::onMktBidAsk(const Ats::OrderBook::CBidAsk & objBidAsk) {

	if (m_bEventQEnabled == false)
		return true;

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::BID_ASK);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push bid-ask");
		return false;
	}

	bRet = m_EventBidAskQ.push(objBidAsk);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push bid-ask");
#else
	bool bRet = m_EventQ.push(objBidAsk);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push bid-ask");
#endif

	return bRet;
}


bool CAlgorithm::onMktQuote(const Ats::OrderBook::CQuote & objQuote) {

	if (m_bEventQEnabled == false)
		return true;

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::QUOTE);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push quote");
		return false;
	}

	bRet = m_EventQuoteQ.push(objQuote);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push quote");
#else
	bool bRet = m_EventQ.push(objQuote);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push quote");
#endif

	return bRet;
}


bool CAlgorithm::onMktOrderBook(const Ats::OrderBook::CConsolidatedBook & objConsolidatedBook) {

	if (m_bEventQEnabled == false)
		return true;

	if (objConsolidatedBook.getTimestamp().getValue() - m_dtLastUpdate[objConsolidatedBook.getSymbolID()].getValue() < m_dUpdateIntervalDayFrac) {

		return true;
	}

	m_dtLastUpdate[objConsolidatedBook.getSymbolID()] = objConsolidatedBook.getTimestamp();

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::CONSOLIDATED_ORDER_BOOK);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push consolidated book");
		return false;
	}

	bRet = m_EventOrderBookQ.push(objConsolidatedBook);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push consolidated book");
#else
	bool bRet = m_EventQ.push(objConsolidatedBook);

	if (bRet == false) {

		m_dUpdateIntervalDayFrac = m_dThrottledUpdateIntervalDayFrac;

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push consolidated book");
	}
	else {

		m_dUpdateIntervalDayFrac = m_dDefaultUpdateIntervalDayFrac;
	}
#endif

	return bRet;
}


bool CAlgorithm::onMktTrade(const Ats::OrderBook::CTrade & objTrade) {

	if (m_bEventQEnabled == false)
		return true;

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::MKT_TRADE);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push trade");
		return false;
	}

	bRet = m_EventTradeQ.push(objTrade);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push trade");
#else
	bool bRet = m_EventQ.push(objTrade);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push trade");
#endif

	return bRet;
}


bool CAlgorithm::onOrderStatus(const Ats::Order::COrderStatus & objOrderStatus) {

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::ORDER_STATUS);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push order status");
		return false;
	}

	bRet = m_EventOrderStatusQ.push(objOrderStatus);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push order status");
#else
	bool bRet = m_EventQ.push(objOrderStatus);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push order status");
#endif

	return bRet;
}


bool CAlgorithm::onOrderFill(const Ats::Order::COrderFill & objOrderFill) {

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::FILL);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push order fill");
		return false;
	}

	bRet = m_EventOrderFillQ.push(objOrderFill);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push order fill");
#else
	bool bRet = m_EventQ.push(objOrderFill);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push order fill");
#endif

	return bRet;
}


bool CAlgorithm::onExecReport(const Ats::Order::CExecReport & objExecReport) {

	bool bRet = m_EventQ.push(objExecReport);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push exec report");

	return bRet;
}


bool CAlgorithm::onDateTime(const Ats::Core::CDateTimeEvent & objDateTime) {

#ifdef LOCKFREE
	bool bRet = m_EventIDQ.push(Ats::Event::CEvent::DATETIME);

	if (bRet == false) {

		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event id queue is full, can't push time event");
		return false;
	}

	bRet = m_EventDateTimeQ.push(objDateTime);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push time event");
#else
	bool bRet = m_EventQ.push(objDateTime);

	if (bRet == false)
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm event queue is full, can't push time event");
#endif

	return bRet;
}


unsigned int CAlgorithm::getAlgoID() const {

	return m_uiAlgoID;
}


const std::string & CAlgorithm::getAlgoName() const {

	return m_strAlgoName;
}


bool CAlgorithm::isStopped() const {

	return m_bStopped;
}


const Ats::Accounting::CPosition & CAlgorithm::getPosition(const Ats::Core::CSymbol & objSymbol) const {

	return m_PositionManager.getPosition(objSymbol);
}


void CAlgorithm::setPosition(const Ats::Core::CSymbol & objSymbol,
														 const Ats::Accounting::CPosition & objPosition) {

	m_PositionManager.setPosition(objSymbol, objPosition);
	m_Residuals.update(objSymbol, objPosition.getPosition(), objPosition.getWAP());
}


bool CAlgorithm::isTradedInstrument(unsigned int iSymbolID) {

	return m_TradedInstrumentMap[iSymbolID] == 1;
}


void CAlgorithm::addTradedInstrument(const Ats::Core::CSymbol & objSymbol) {

	m_TradedInstruments.push_back(objSymbol);
	m_TradedInstrumentMap[objSymbol.getSymbolID()] = 1;
}


void CAlgorithm::addAlgorithmSubscriber(IAlgorithmSubscriber * ptrAlgoritmSubscriber) {

	m_AlgoSubscribers.push_back(ptrAlgoritmSubscriber);
}


void CAlgorithm::broadcastMessage(const std::string & strMsg) const {

	AlgorithmSubscriberArray::const_iterator iter = m_AlgoSubscribers.begin();

	for (; iter != m_AlgoSubscribers.end(); ++iter)
		(*iter)->onAlgorithmMessage(strMsg);
}


void CAlgorithm::broadcastRiskBreach(unsigned int iAlgoID) const {

	AlgorithmSubscriberArray::const_iterator iter = m_AlgoSubscribers.begin();

	for (; iter != m_AlgoSubscribers.end(); ++iter)
		(*iter)->onAlgorithmRiskBreach(iAlgoID);
}


void CAlgorithm::broadcastInternalError(const std::string & strMsg) const {

	AlgorithmSubscriberArray::const_iterator iter = m_AlgoSubscribers.begin();

	for (; iter != m_AlgoSubscribers.end(); ++iter)
		(*iter)->onAlgorithmInternalError(m_uiAlgoID, strMsg);
}


void CAlgorithm::onCreate() {

	char szBuffer[255];
	sprintf(szBuffer, "%s//%s_log_%llu.txt", Ats::Core::CConfig::getInstance().getLogFileDirectory().c_str(), getAlgoName().c_str(), Ats::Core::CDateTime::now().getCMTime());
	
	m_Logger.setLogFile(szBuffer);
	m_Logger.startLogging();
	m_Logger.log("CAlgorithm::onCreate()\n");
}


void CAlgorithm::onEvent(const Ats::Order::COrderFill & objOrderFill) {

	m_PositionManager.updatePosition(objOrderFill);
	m_Residuals.update(objOrderFill);

	const Ats::Core::CSymbol &					objSymbol		= Ats::Core::CSymbol::getSymbol(objOrderFill.getOrigOrder().getSymbolID());
	const Ats::Accounting::CPosition &	objPosition = m_PositionManager.getPosition(objSymbol);

	AlgorithmSubscriberArray::const_iterator iter = m_AlgoSubscribers.begin();

	for (; iter != m_AlgoSubscribers.end(); ++iter)
		(*iter)->onAlgorithmPositionUpdate(m_uiAlgoID, objPosition);

	// post execution risk checks
	enforceMaxPositionBaseCcyLimit(objPosition, objSymbol);
	enforceMaxOrderSizeBaseCcyLimit(objOrderFill, objSymbol);
	enforceMinTradeInterval(objOrderFill, objSymbol);

	if (m_DrawdownControl.useDDC() == true) {
	
		// is this a DDC generated order
		const char * pszOrderTag = objOrderFill.getOrigOrder().getOrderTag();

		if (*pszOrderTag == 'D') {

			m_DrawdownControl.onFill(objOrderFill);
		}
	}
}


void CAlgorithm::onRevalSnap(unsigned int									iSymbolID,
														 const Ats::Core::CDateTime & dtTimestamp,
														 int													iDirection,
														 double												dMidPx) {

	// store execution details for optional reval
	m_dLastExecPx[iSymbolID]				= dMidPx;
	m_dtLastExecTime[iSymbolID]			= dtTimestamp.getValue();
	m_dLastExecDirection[iSymbolID]	= iDirection;

	for (size_t i = 0; i != m_RevalIntSecsArray.size(); ++i) {

		m_Revals[i][iSymbolID] = 0.0;
	}
}


void CAlgorithm::onDestroy() {

	try {
		m_Logger.stopLogging();
		m_Logger.log("CAlgorithm::onDestroy()\n");
	}
	catch(std::exception & e) {
		Ats::Log::CApplicationLog::getInstance().log(e.what());
	}
}


void CAlgorithm::onStart() {

	m_bStopped = false;
}


void CAlgorithm::onStartOfDay(Ats::Core::CDateTime) {

}


void CAlgorithm::onEngineStart(const Ats::TickData::CPriceMatrix &) {

}


void CAlgorithm::onEngineStart(const Ats::TickData::CPriceMatrix &,
															 const Ats::TickData::CPriceMatrix &) {

}


// this method should be called at the end of any algo specific override due to the latency overhead
void CAlgorithm::onEvent(const Ats::OrderBook::CConsolidatedBook & objConsolidatedBook) {

	const Ats::Core::CSymbol &	objSymbol = Ats::Core::CSymbol::getSymbol(objConsolidatedBook.getSymbolID());
	const Ats::Core::CCurrncy & objRefCcy = objSymbol.getRefCcy();
	double											dBestMid	= 0.5 * objConsolidatedBook.getBestBid().getPrice() + 0.5 * objConsolidatedBook.getBestAsk().getPrice();

	if (dBestMid <= 0 || objConsolidatedBook.getBestBid().getPrice() <= 0 || objConsolidatedBook.getBestAsk().getPrice() <= 0)
		return;

	// rolling spread
	double dSpreadBps = 10000.0 * ((objConsolidatedBook.getBestAsk().getPrice() - objConsolidatedBook.getBestBid().getPrice()) / dBestMid);

	m_RollingSpreadHistFast[objSymbol.getSymbolID()].append(objConsolidatedBook.getExcTimestamp(), dSpreadBps);
	m_RollingSpreadHistSlow[objSymbol.getSymbolID()].append(objConsolidatedBook.getExcTimestamp(), dSpreadBps);
	m_SpreadMAHist[objSymbol.getSymbolID()][(objConsolidatedBook.getExcTimestamp().getDayOfWeek() - 1) * MINUTES_PER_DAY + objConsolidatedBook.getExcTimestamp().getTimeInMin()] = m_RollingSpreadHistSlow[objSymbol.getSymbolID()].getEWMA();

	// rolling ATR
	m_RollingATR[objSymbol.getSymbolID()].update(objConsolidatedBook.getExcTimestamp(), dBestMid);

	static double UPDATE_INTERVAL_DAYFRAC = static_cast<double>(m_iUpdateMSecPnL) / (24.0 * 60.0 * 60.0 * 1000.0);

	if (objConsolidatedBook.getExcTimestamp().getValue() - m_dtLastUpdatePnL[objConsolidatedBook.getSymbolID()].getValue() < UPDATE_INTERVAL_DAYFRAC) {

		return;
	}

	bool bNewDay = objConsolidatedBook.getExcTimestamp().equalDay(m_dtLastUpdatePnL[objConsolidatedBook.getSymbolID()]) == false;

	m_dtLastUpdatePnL[objConsolidatedBook.getSymbolID()] = objConsolidatedBook.getExcTimestamp();

	m_Residuals.update(objSymbol, dBestMid);

	// track max pnl for trailing stop
	m_dCurrPnLUSD																	= m_Residuals.getTotalUSDPnL();
	m_dMaxPnLUSD																	= std::max(m_dMaxPnLUSD, m_dCurrPnLUSD);
	m_dCurrPairPnLUSD[objSymbol.getSymbolID()]		= m_PositionManager.getPairPnLRefCcy(objSymbol, dBestMid) * m_Residuals.getBase2USDExcRate(objSymbol.getRefCcy());
	m_dMaxPairPnLUSD[objSymbol.getSymbolID()]			= std::max(m_dMaxPairPnLUSD[objSymbol.getSymbolID()], m_dCurrPairPnLUSD[objSymbol.getSymbolID()]);
	m_dCurrCcyPnLUSD[objRefCcy.getCcyID()]				= m_Residuals.getTotalUSDPnL(objRefCcy.getCcyID());
	m_dMaxCcyPnLUSD[objRefCcy.getCcyID()]					= std::max(m_dMaxCcyPnLUSD[objRefCcy.getCcyID()], m_dCurrCcyPnLUSD[objRefCcy.getCcyID()]);

	// drawdown control
	m_DrawdownControl.update(objSymbol.getSymbolID(), m_dCurrPairPnLUSD[objSymbol.getSymbolID()]);
	m_DrawdownControl.update(m_dCurrPnLUSD);

	if (bNewDay == true) {

		m_dMaxPnLUSD																			= m_dCurrPnLUSD;
		m_dStartOfDayPnLUSD																= m_dCurrPnLUSD;
		m_dMaxPairPnLUSD[objSymbol.getSymbolID()]					= m_dCurrPairPnLUSD[objSymbol.getSymbolID()];
		m_dStartOfDayPairPnLUSD[objSymbol.getSymbolID()]	= m_dCurrPairPnLUSD[objSymbol.getSymbolID()];
		m_dMaxCcyPnLUSD[objRefCcy.getCcyID()]							= m_dCurrCcyPnLUSD[objRefCcy.getCcyID()];
		m_dStartOfDayCcyPnLUSD[objRefCcy.getCcyID()]			= m_dCurrCcyPnLUSD[objRefCcy.getCcyID()];

		m_DrawdownControl.reset(objSymbol.getSymbolID(), m_dCurrPairPnLUSD[objSymbol.getSymbolID()]);
		m_DrawdownControl.reset(m_dCurrPnLUSD);
	}

	broadcastPnL();
}


void CAlgorithm::onEvent(const Ats::OrderBook::CBidAsk &) {

}


void CAlgorithm::onEvent(const Ats::OrderBook::CQuote &) {

}


void CAlgorithm::onEvent(const Ats::OrderBook::CTrade &) {

}


void CAlgorithm::onEvent(const Ats::Order::COrderStatus &) {

}


void CAlgorithm::onEvent(const Ats::Order::CExecReport &) {

}


void CAlgorithm::onEvent(const std::string &) {

}


void CAlgorithm::onEvent(const Ats::Core::CDateTimeEvent &) {

}


void CAlgorithm::onEndOfDay(Ats::Core::CDateTime) {

}


void CAlgorithm::onProviderActive(unsigned int iProviderID) {

	if (m_ProviderStatus.find(iProviderID) == m_ProviderStatus.end()) {
		m_ProviderStatus.insert(std::pair<unsigned int, bool>(iProviderID, true));
	}
	else {
		m_ProviderStatus[iProviderID] = true;
	}
}


void CAlgorithm::onProviderInactive(unsigned int iProviderID) {

	if (m_ProviderStatus.find(iProviderID) == m_ProviderStatus.end()) {
		m_ProviderStatus.insert(std::pair<unsigned int, bool>(iProviderID, false));
	}
	else {
		m_ProviderStatus[iProviderID] = false;
	}
}


bool CAlgorithm::isExchangeAvailable(unsigned int iProviderID) const {

	ProviderAvailabilityMap::const_iterator iter = m_ProviderStatus.find(iProviderID);

	if (iter == m_ProviderStatus.end()) {
		return false;
	}
	else {
		return iter->second;
	}
}


void CAlgorithm::onStop() {

	m_bStopped = true;
}


std::vector<std::string> CAlgorithm::getMessageHistory() const {

	std::vector<std::string> objMessages;
	return objMessages;
}


std::vector<std::string> CAlgorithm::getState() const {

	std::vector<std::string> objMessages;

	for (size_t i = 0; i != m_TradedInstruments.size(); ++i) {

		objMessages.push_back(buildPositionMessage(m_TradedInstruments[i]));
	}

	objMessages.push_back(buildPnLUSDMessage());

	return objMessages;
}


void CAlgorithm::broadcastPosition(const Ats::Core::CSymbol & objSymbol) const {

	broadcastMessage(buildPositionMessage(objSymbol));
}


void CAlgorithm::broadcastPnL() const {

	broadcastMessage(buildPnLUSDMessage());
}


double CAlgorithm::getPnLUSD() const {

	return m_Residuals.getTotalUSDPnL();
}


std::string CAlgorithm::buildPositionMessage(const Ats::Core::CSymbol & objSymbol) const {

	double dPosition			= getPosition(objSymbol).getPosition();
	double dPosBaseCcyUSD = m_Residuals.getUSDPosition(objSymbol.getBaseCcy());
	double dPosRefCcyUSD	= m_Residuals.getUSDPosition(objSymbol.getRefCcy());

	char szBuffer[512];
	sprintf(szBuffer, "ALGO_POSITION,%d,%s,%d,%s,%d,%s,%d", getAlgoID(), objSymbol.getSymbol().c_str(), static_cast<int>(dPosition), objSymbol.getBaseCcy().getCcy().c_str(), static_cast<int>(dPosBaseCcyUSD), objSymbol.getRefCcy().getCcy().c_str(), static_cast<int>(dPosRefCcyUSD));
	
	return szBuffer;
}


std::string CAlgorithm::buildPnLUSDMessage() const {

	double dPnLUSD = m_Residuals.getTotalUSDPnL();

	char szBuffer[512];
	sprintf(szBuffer, "ALGO_PNL,%d,%d", getAlgoID(), static_cast<int>(dPnLUSD));

	return szBuffer;
}


double CAlgorithm::getUsdPosition() const {

	static Ats::Core::CCurrncy objUSD = Ats::Core::CCurrncy::getCcy("USD");

	return m_Residuals.getUSDPosition(objUSD);
}


double CAlgorithm::getCcyUsdPosition(const Ats::Core::CCurrncy & objBaseCcy) const {

	return m_Residuals.getUSDPosition(objBaseCcy);
}


std::string CAlgorithm::getAlgoInitString() const {

	return "";
}


// computes projected ccy exposure in the event a hypothetical order is executed
void CAlgorithm::getProjectedCcyUsdPositions(const Ats::Core::CSymbol &		objSymbol,
																						 Ats::Order::COrder::BuySell	iDirection,
																						 unsigned int									iLotSize,
																						 double												dMidPx,
																						 double &											dProjUsdPosBase,
																						 double &											dProjUsdPosRef) const {

	double dCurrPosBase = m_Residuals.getPosition(objSymbol.getBaseCcy());
	double dCurrPosRef	= m_Residuals.getPosition(objSymbol.getRefCcy());

	double dProjPosBase = dCurrPosBase;
	double dProjPosRef	= dCurrPosRef;

	if (iDirection == Ats::Order::COrder::BUY) {

		dProjPosBase	+= iLotSize;
		dProjPosRef		-= iLotSize * dMidPx;
	}
	else {

		dProjPosBase	-= iLotSize;
		dProjPosRef		+= iLotSize * dMidPx;
	}

	dProjUsdPosBase = dProjPosBase * objSymbol.getBaseCcy().getCcy2USDExcRate();
	dProjUsdPosRef	= dProjPosRef * objSymbol.getRefCcy().getCcy2USDExcRate();
}


void CAlgorithm::setMaxPositionBaseCcy(const Ats::Core::CSymbol & objSymbol, 
																			 int												iMaxPositionBaseCcy) {

	m_iMaxPositionBaseCcy[objSymbol.getSymbolID()] = iMaxPositionBaseCcy;
}


void CAlgorithm::setMaxOrderSizeBaseCcy(const Ats::Core::CSymbol & objSymbol, 
																				int												 iMaxOrderSizeBaseCcy) {

	m_iMaxOrderSizeBaseCcy[objSymbol.getSymbolID()] = iMaxOrderSizeBaseCcy;
}


void CAlgorithm::setMinTradeIntervalSecs(int iMinTradeIntervalInSecs) {

	m_dMinTradeIntervalDayFrac = static_cast<double>(iMinTradeIntervalInSecs) / (24.0 * 60.0 * 60.0);
}


void CAlgorithm::setUseTrailingStop(bool bUseTrailingStop) {

	m_bUseTrailingStop = bUseTrailingStop;
}


void CAlgorithm::setDailyMaxLoss(double dDailyMaxLoss) {

	m_dDailyMaxLoss = dDailyMaxLoss;
}


void CAlgorithm::setDailyProfitThreshold1(double dDailyProfitThreshold) {

	m_dDailyProfitThresholdUSD1 = dDailyProfitThreshold;
}


void CAlgorithm::setDailyProfitLock1(double dDailyProfitLock) {

	m_dProfitLockUSD1 = dDailyProfitLock;
}


void CAlgorithm::setDailyProfitThreshold2(double dDailyProfitThreshold) {

	m_dDailyProfitThresholdUSD2 = dDailyProfitThreshold;
}


void CAlgorithm::setDailyProfitLock2(double dDailyProfitLock) {

	m_dProfitLockUSD2 = dDailyProfitLock;
}


// after the event check which will shut down the algo if limit breached. backup to higher level pre-event checks.
void CAlgorithm::enforceMaxPositionBaseCcyLimit(const Ats::Accounting::CPosition &	objPosition,
																								const Ats::Core::CSymbol &					objSymbol) {

	if (abs(static_cast<int>(objPosition.getPosition())) > m_iMaxPositionBaseCcy[objSymbol.getSymbolID()]) {

		setOperationalMode(PASSIVE);
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm has breached position limit for " + objSymbol.getSymbol() + " and has been set to PASSIVE");
		broadcastRiskBreach(m_uiAlgoID);
	}
}


// after the event check which will shut down the algo if limit breached. backup to higher level pre-event checks.
void CAlgorithm::enforceMaxOrderSizeBaseCcyLimit(const Ats::Order::COrderFill &	objOrderFill,
																								 const Ats::Core::CSymbol &			objSymbol) {

	if (abs(static_cast<int>(objOrderFill.getOrigOrder().getQuantity())) > m_iMaxOrderSizeBaseCcy[objSymbol.getSymbolID()]) {

		setOperationalMode(PASSIVE);
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm has breached order size limit for " + objSymbol.getSymbol() + " and has been set to PASSIVE");
		broadcastRiskBreach(m_uiAlgoID);
	}
}


// after the event check which will shut down the algo if trade throttle breached. backup to higher level pre-event checks.
void CAlgorithm::enforceMinTradeInterval(const Ats::Order::COrderFill &	objOrderFill,
																				 const Ats::Core::CSymbol &			objSymbol) {

	if (objOrderFill.getFillTimestamp().getValue() - m_dtLastFillTime[objSymbol.getSymbolID()].getValue() < m_dMinTradeIntervalDayFrac) {

		setOperationalMode(PASSIVE);
		Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm has breached order throttle for " + objSymbol.getSymbol() + " and has been set to PASSIVE");
		broadcastRiskBreach(m_uiAlgoID);
	}

	m_dtLastFillTime[objSymbol.getSymbolID()] = objOrderFill.getFillTimestamp();
}


// pre-order submission check, must be called explicitly by derived class
bool CAlgorithm::isOrderCompliant(Ats::Order::COrder & objOrder) {

	const Ats::Core::CSymbol & objSymbol = Ats::Core::CSymbol::getSymbol(objOrder.getSymbolID());

	long iCurrPos				= getPosition(objSymbol).getPosition();

	bool bLimitBreach		= static_cast<int>(objOrder.getQuantity()) > m_iMaxOrderSizeBaseCcy[objSymbol.getSymbolID()] ||
												iCurrPos + (objOrder.getDirection() == Ats::Order::COrder::BUY ? 1 : -1) * static_cast<int>(objOrder.getQuantity()) > m_iMaxPositionBaseCcy[objSymbol.getSymbolID()];

	bool bMinSizeBreach	=	objOrder.getQuantity() == 0;

	bool bPnLBreach			= isPnLStopHit();

	if (bLimitBreach == true || bMinSizeBreach || bPnLBreach) {

		if (bPnLBreach) {
			setOperationalMode(LIQUIDATE);
			Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm PnL stop loss hit for " + objSymbol.getSymbol() + " and has been set to LIQUIDATE");
		}
		else {
			setOperationalMode(PASSIVE);
			Ats::Log::CApplicationLog::getInstance().log(m_strAlgoName + " - Algorithm has attempted to breach position or order size limit for " + objSymbol.getSymbol() + " and has been set to PASSIVE");
			broadcastRiskBreach(m_uiAlgoID);
		}
	}

	return bLimitBreach == false;
}


void CAlgorithm::enableEventQueue() {

	m_bEventQEnabled = true;
}


void CAlgorithm::disableEventQueue() {

	m_bEventQEnabled = false;

#ifdef LOCKFREE
	m_EventIDQ.reset();
	m_EventOrderBookQ.reset();
	m_EventBidAskQ.reset();
	m_EventQuoteQ.reset();
	m_EventTradeQ.reset();
	m_EventOrderStatusQ.reset();
	m_EventOrderFillQ.reset();
	m_EventDateTimeQ.reset();
#else
	m_EventQ.reset();
#endif
}


bool CAlgorithm::isPnLStopHit() const {

	bool bDefaultSL	= false;

	if (m_bUseTrailingStop == true)
		bDefaultSL = m_dDailyMaxLoss < 0 && m_dCurrPnLUSD - m_dMaxPnLUSD <= m_dDailyMaxLoss;
	else
		bDefaultSL = m_dDailyMaxLoss < 0 && m_dCurrPnLUSD <= m_dDailyMaxLoss;

	bool bTakeProfitSL1 = m_dDailyProfitThresholdUSD1 > 0 && m_dMaxPnLUSD > m_dDailyProfitThresholdUSD1 && m_dCurrPnLUSD - m_dMaxPnLUSD <= m_dProfitLockUSD1;
	bool bTakeProfitSL2 = m_dDailyProfitThresholdUSD2 > 0 && m_dMaxPnLUSD > m_dDailyProfitThresholdUSD2 && m_dCurrPnLUSD - m_dMaxPnLUSD <= m_dProfitLockUSD2;

	//bool bTakeProfitSL1 = m_dDailyProfitThresholdUSD1 > 0 && m_dMaxPnLUSD - m_dStartOfDayPnLUSD > m_dDailyProfitThresholdUSD1 && m_dCurrPnLUSD - m_dMaxPnLUSD <= m_dProfitLockUSD1;
	//bool bTakeProfitSL2 = m_dDailyProfitThresholdUSD2 > 0 && m_dMaxPnLUSD - m_dStartOfDayPnLUSD > m_dDailyProfitThresholdUSD2 && m_dCurrPnLUSD - m_dMaxPnLUSD <= m_dProfitLockUSD2;

	return bDefaultSL == true || bTakeProfitSL1 == true || bTakeProfitSL2 == true;
}


void CAlgorithm::resetStop() {

	m_dMaxPnLUSD = m_dCurrPnLUSD;
}


void CAlgorithm::setEventProcessor(const Ats::Indicator::CEventProcessor & objEventProcessor) {

	m_EventProcessor = objEventProcessor;
}


double CAlgorithm::getDailyDD() const {

	return m_dCurrPnLUSD - m_dMaxPnLUSD;
}


double CAlgorithm::getDailyPnL() const {

	return m_dCurrPnLUSD - m_dStartOfDayPnLUSD;
}


bool CAlgorithm::isConcentrationLimitBreached(const Ats::Core::CSymbol &	objSymbol,
																							Ats::Order::COrder::BuySell	iDirection,
																							unsigned int								iQuantityUSD,
																							unsigned int								iCcyPosLimitUSD) const {

	double dCurrPosBaseCcyUSD = CAlgorithm::getCcyUsdPosition(objSymbol.getBaseCcy());
	double dCurrPosRefCcyUSD	= CAlgorithm::getCcyUsdPosition(objSymbol.getRefCcy());

	double dNewPosBaseCcyUSD  = dCurrPosBaseCcyUSD + (iDirection == Ats::Order::COrder::BUY ? 1 : -1) * static_cast<double>(iQuantityUSD);
	double dNewPosRefCcyUSD		= dCurrPosRefCcyUSD - (iDirection == Ats::Order::COrder::BUY ? 1 : -1) * static_cast<double>(iQuantityUSD);

	return fabs(dNewPosBaseCcyUSD) > iCcyPosLimitUSD || fabs(dNewPosRefCcyUSD) > iCcyPosLimitUSD;
}


void CAlgorithm::setPortfolioConstraintManager(const Ats::Risk::CPortfolioConstraintManager & objPortfolioConstraintManager) {

	m_PortfolioConstraintManager = objPortfolioConstraintManager;
}


bool CAlgorithm::isPortfolioStopHit() const {

	return m_bPortfolioStopHit;
}


void CAlgorithm::setPortfolioStopHit(bool bPortfolioStopHit) {

	m_bPortfolioStopHit = bPortfolioStopHit;

	std::string strMsg = bPortfolioStopHit == true ? "Portfolio stop: true" : "Portfolio stop: false";
	Ats::Log::CApplicationLog::getInstance().log(strMsg);
}


void CAlgorithm::applyRiskMultiplier(double dMultiplier) {

	for (int i = 0; i != Ats::Core::CConfig::MAX_NUM_SYMBOLS; ++i) {

		m_iMaxPositionBaseCcy[i]	= static_cast<int>(static_cast<double>(m_iMaxPositionBaseCcy[i]) * dMultiplier);
		m_iMaxOrderSizeBaseCcy[i] = static_cast<int>(static_cast<double>(m_iMaxOrderSizeBaseCcy[i]) * dMultiplier);
	}
}


double CAlgorithm::getPairPnLUSD(const Ats::Core::CSymbol & objSymbol) const {

	return m_dCurrPairPnLUSD[objSymbol.getSymbolID()] - m_dStartOfDayPairPnLUSD[objSymbol.getSymbolID()];
}


double CAlgorithm::getPairDDUSD(const Ats::Core::CSymbol & objSymbol) const {

	return m_dCurrPairPnLUSD[objSymbol.getSymbolID()] - m_dMaxPairPnLUSD[objSymbol.getSymbolID()];
}


double CAlgorithm::getDD() const {

	return m_DrawdownControl.getDrawdown();
}


double CAlgorithm::getDDC() const {

	return m_DrawdownControl.getRiskMultiplier();
}


void CAlgorithm::onFillDDC(const Ats::Order::COrderFill & objOrderFill) {

	m_DrawdownControl.onFill(objOrderFill);
}


bool CAlgorithm::isDDCAdjustmentRequired() const {

	return m_DrawdownControl.isAdjustmentRequired();
}


double CAlgorithm::getDDCMaxSpreadBps() const {

	return m_DrawdownControl.getMaxSpreadBps();
}


bool CAlgorithm::useDDC() const {

	return m_DrawdownControl.useDDC();
}


void CAlgorithm::setDDCParameters(double dNotionalCapitalUSD,
																	double dMinRiskMultiplier,
																	double dModelThresholdDD,
																	double dModelMaxDrawdown,
																	double dModelRiskDecay,
																	double dSymbolThresholdDD,
																	double dSymbolMaxDrawdown,
																	double dSymbolRiskDecay,
																	double dAdjustmentDelta,
																	bool	 bDailyReset,
																	double dMaxSpreadBps,
																	bool	 bUseDDC) {

	m_DrawdownControl.setParameters(dNotionalCapitalUSD, dMinRiskMultiplier, dModelThresholdDD, dModelMaxDrawdown, dModelRiskDecay, dSymbolThresholdDD, dSymbolMaxDrawdown, dSymbolRiskDecay, dAdjustmentDelta, bDailyReset, dMaxSpreadBps, bUseDDC);
}


double CAlgorithm::getAverageSpreadBps(unsigned int									iSymbolID,
																			 const Ats::Core::CDateTime & dtNow) {

	return m_RollingSpreadHistFast[iSymbolID].getEWMA();// m_SpreadMAHistFast[iSymbolID][(dtNow.getDayOfWeek() - 1)*MINUTES_PER_DAY + dtNow.getTimeInMin()];
}


double CAlgorithm::getAverageSpreadFastBps(unsigned int iSymbolID) {

	return m_RollingSpreadHistFast[iSymbolID].getEWMA();
}


double CAlgorithm::getAverageSpreadSlowBps(unsigned int iSymbolID) {

	return m_RollingSpreadHistSlow[iSymbolID].getEWMA();
}


double CAlgorithm::getExpectedSpreadBps(unsigned int								 iSymbolID,
																				const Ats::Core::CDateTime & dtNow) {

	double dWeeklyMA = (m_SpreadMAHist[iSymbolID][0 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
											m_SpreadMAHist[iSymbolID][1 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
											m_SpreadMAHist[iSymbolID][2 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
											m_SpreadMAHist[iSymbolID][3 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
											m_SpreadMAHist[iSymbolID][4 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
											m_SpreadMAHist[iSymbolID][5 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
											m_SpreadMAHist[iSymbolID][6 * MINUTES_PER_DAY + dtNow.getTimeInMin()]) / static_cast<double>(DAYS_PER_WEEK);

	return dWeeklyMA;
}


// returns ratio of current (smoothed) spread to the prior day(s) spread (smoothed) for the equivalent time
double CAlgorithm::getSpreadRatio(unsigned int								 iSymbolID,
																	const Ats::Core::CDateTime & dtNow) {

	double dWeeklyMA = (m_SpreadMAHist[iSymbolID][0 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
										  m_SpreadMAHist[iSymbolID][1 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
										  m_SpreadMAHist[iSymbolID][2 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
										  m_SpreadMAHist[iSymbolID][3 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
										  m_SpreadMAHist[iSymbolID][4 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
										  m_SpreadMAHist[iSymbolID][5 * MINUTES_PER_DAY + dtNow.getTimeInMin()] +
										  m_SpreadMAHist[iSymbolID][6 * MINUTES_PER_DAY + dtNow.getTimeInMin()]) / static_cast<double>(DAYS_PER_WEEK);

	double dSpreadRatio = fabs(dWeeklyMA) > 0.0 ? getAverageSpreadBps(iSymbolID, dtNow) / dWeeklyMA : 1.0;

	return dSpreadRatio;
}


void CAlgorithm::revalLastExecution(unsigned int									iSymbolID,
																		const Ats::Core::CDateTime &	dtNow,
																		double												dMidPx) {

	if (m_dLastExecPx[iSymbolID] <= 0)
		return;

	char szBuffer[255];

	const Ats::Core::CSymbol &	objSymbol = Ats::Core::CSymbol::getSymbol(iSymbolID);

	for (size_t i = 0; i != m_RevalIntSecsArray.size(); ++i) {

		if (fabs(m_Revals[i][iSymbolID]) <= 0 && dtNow.getValue() - m_dtLastExecTime[iSymbolID].getValue() > m_RevalIntDayFracArray[i]) {

			m_Revals[i][iSymbolID] = m_dLastExecDirection[iSymbolID] * (log(dMidPx) - log(m_dLastExecPx[iSymbolID]));

			sprintf(szBuffer, "%llu %llu Reval %.2f sec %s %.2f", dtNow.getCMTime(), m_dtLastExecTime[iSymbolID].getCMTime(), m_RevalIntSecsArray[i], objSymbol.getSymbol().c_str(), m_Revals[i][iSymbolID] * 10000.0);
			m_Logger.log(szBuffer);
		}
	}
}


double CAlgorithm::getATR(unsigned int iSymbolID) {

	return m_RollingATR[iSymbolID].getATR();
}


bool CAlgorithm::increasingATR(unsigned int iSymbolID) {

	return m_RollingATR[iSymbolID].isIncreasing();
}



