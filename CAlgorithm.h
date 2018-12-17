//===============================================================================
// Copyright (c) 2018, Optimal Alpha LLC
//===============================================================================

#ifndef CALGORITHM_HEADER

#define CALGORITHM_HEADER

#include <string>
#include <boost/thread/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/unordered_map.hpp>
#include "CSymbol.h"
#include "CConsolidatedBook.h"
#include "CBidAsk.h"
#include "COrderStatus.h"
#include "COrderFill.h"
#include "CTrade.h"
#include "IRunnable.h"
#include "CEvent.h"
#include "CEventQueue.h"
#include "CLog.h"
#include "CPositionManager.h"
#include "IAlgorithmSubscriber.h"
#include "CResidualsManager.h"
#include "CPriceMatrix.h"
#include "CDateTimeEvent.h"
#include "CEventProcessor.h"
#include "CEventQueueLockFree.h"
#include "CPortfolioConstraintManager.h"
#include "CExecReport.h"
#include "CDrawdownControl.h"
#include "CTimedCircularScalarBuffer.h"
#include "CRollingATR.h"

namespace Ats
{
	namespace Algorithm
	{
		class CAlgorithm : public Ats::Thread::IRunnable
		{
		public:
			enum OperationalMode { PASSIVE,
														 LIQUIDATE,
														 ACTIVE };
		public:
			CAlgorithm(unsigned int					uiAlgoID,
								 const std::string &	strAlgoName,
								 unsigned int					uiEventBufferSizeBytes,
								 unsigned int					uiUpdateMSec);

			// main event loop will run on a separate thread
			void run();
			void operator()();

			// used to enable/disable event queue to market updates, order updates always go through (for MATLAB models to prevent queue filling up)
			void enableEventQueue();
			void disableEventQueue();

			// place items in the event queue (engine will call these methods, once queued the algo can process the events in it's own time)
			bool onMktBidAsk(const Ats::OrderBook::CBidAsk & objBidAsk);
			bool onMktQuote(const Ats::OrderBook::CQuote & objQuote);
			bool onMktOrderBook(const Ats::OrderBook::CConsolidatedBook & objConsolidatedBook);
			bool onMktTrade(const Ats::OrderBook::CTrade & objTrade);
			bool onOrderStatus(const Ats::Order::COrderStatus & objOrderStatus);
			bool onOrderFill(const Ats::Order::COrderFill & objOrderFill);
			bool onDateTime(const Ats::Core::CDateTimeEvent & objDateTime);
			bool onExecReport(const Ats::Order::CExecReport & objExecReport);
			void onRevalSnap(unsigned int									iSymbolID,
											 const Ats::Core::CDateTime & dtTimestamp,
											 int													iDirection,
											 double												dMidPx);

			// this method will construct the next event from the event queue and call the overloaded onEvent() method to handle it
			template <typename T>
			void handleEvent(Ats::Event::CEvent::EventID iEventID);

			// accessors
			unsigned int getAlgoID() const;
			const std::string & getAlgoName() const;
			bool isStopped() const;
			const Ats::Accounting::CPosition & getPosition(const Ats::Core::CSymbol & objSymbol) const;
			void setPosition(const Ats::Core::CSymbol & objSymbol,
											 const Ats::Accounting::CPosition & objPosition);
			OperationalMode getOperationalMode() const;
			void setOperationalMode(OperationalMode iOperationalMode);
			double getPnLUSD() const;
			double getUsdPosition() const;
			double getCcyUsdPosition(const Ats::Core::CCurrncy & objBaseCcy) const;
			double getPairPnLUSD(const Ats::Core::CSymbol & objSymbol) const;
			double getPairDDUSD(const Ats::Core::CSymbol & objSymbol) const;

			void getProjectedCcyUsdPositions(const Ats::Core::CSymbol &		objSymbol,
																			 Ats::Order::COrder::BuySell	iDirection,
																			 unsigned int									iLotSize,
																			 double												dMidPx,
																			 double &											dProjUsdPosBase,
																			 double &											dProjUsdPosRef) const;

			bool isTradedInstrument(unsigned int iSymbolID);
			void addTradedInstrument(const Ats::Core::CSymbol & objSymbol);

			double getAverageSpreadBps(unsigned int									iSymbolID,
																 const Ats::Core::CDateTime & dtNow);

			double getSpreadRatio(unsigned int								 iSymbolID,
														const Ats::Core::CDateTime & dtNow);

			double getAverageSpreadFastBps(unsigned int iSymbolID);

			double getAverageSpreadSlowBps(unsigned int iSymbolID);

			double getExpectedSpreadBps(unsigned int								 iSymbolID,
																	const Ats::Core::CDateTime & dtNow);

			double getATR(unsigned int iSymbolID);
			bool increasingATR(unsigned int iSymbolID);

			// manage subscribers
			void addAlgorithmSubscriber(IAlgorithmSubscriber * ptrAlgoritmSubscriber);
			void broadcastMessage(const std::string & strMsg) const;
			void broadcastPosition(const Ats::Core::CSymbol & objSymbol) const;
			void broadcastPnL() const;
			void broadcastRiskBreach(unsigned int iAlgoID) const;
			void broadcastInternalError(const std::string & strMsg) const;
			bool isExchangeAvailable(unsigned int iProviderID) const;
			std::vector<std::string> getState() const;

			// called once when algo is deployed to the engine (resource allocation code goes here)
			virtual void onCreate();

			// called whenever the algo is (re)started by the user
			virtual void onStart();

			// called once daily on the first event
			virtual void onStartOfDay(Ats::Core::CDateTime dtTimestamp);

			// called once, each time the engine starts up, use to initialize state with supplied data history
			virtual void onEngineStart(const Ats::TickData::CPriceMatrix & objPriceHist5Min);

			virtual void onEngineStart(const Ats::TickData::CPriceMatrix & objPriceHist5Min,
																 const Ats::TickData::CPriceMatrix & objPriceHist1Min);

			// handlers for market events
			virtual void onEvent(const Ats::OrderBook::CConsolidatedBook & objConsolidatedBook);
			virtual void onEvent(const Ats::OrderBook::CBidAsk & objBidAsk);
			virtual void onEvent(const Ats::OrderBook::CQuote & objQuote);
			virtual void onEvent(const Ats::OrderBook::CTrade & objTrade);
			virtual void onEvent(const Ats::Order::COrderStatus & objOrderStatus);
			virtual void onEvent(const Ats::Order::COrderFill & objOrderFill);
			virtual void onEvent(const std::string & strUserCommand);
			virtual void onEvent(const Ats::Core::CDateTimeEvent & objTimestamp);
			virtual void onEvent(const Ats::Order::CExecReport & objExecReport);

			// called once daily at the end of the trading day (calendar)
			virtual void onEndOfDay(Ats::Core::CDateTime dtTimestamp);

			// called when connections are established to feeds/exchanges
			virtual void onProviderActive(unsigned int iProviderID);
			virtual void onProviderInactive(unsigned int iProviderID);

			// called whenever the algo is stopped by the user
			virtual void onStop();

			// called once when the algo is removed from the engine (resource cleanup code goes here)
			virtual void onDestroy();

			// returns the messages which have been transmitted from the algorithm to the GUI
			virtual std::vector<std::string> getMessageHistory() const;

			// returns algo specific initialization data to the GUI
			virtual std::string getAlgoInitString() const;

			// risk limits
			void setMaxPositionBaseCcy(const Ats::Core::CSymbol & objSymbol, 
																 int												iMaxPositionBaseCcy);

			void setMaxOrderSizeBaseCcy(const Ats::Core::CSymbol & objSymbol, 
																	int												 iMaxOrderSizeBaseCcy);

			void setMinTradeIntervalSecs(int iMinTradeIntervalInSecs);

			void setUseTrailingStop(bool bUseTrailingStop);
			void setDailyMaxLoss(double dDailyMaxLoss);
			void setDailyProfitThreshold1(double dDailyProfitThreshold);
			void setDailyProfitLock1(double dDailyProfitLock);
			void setDailyProfitThreshold2(double dDailyProfitThreshold);
			void setDailyProfitLock2(double dDailyProfitLock);

			bool isPortfolioStopHit() const;
			void setPortfolioStopHit(bool bPortfolioStopHit);

			void enforceMaxPositionBaseCcyLimit(const Ats::Accounting::CPosition &	objPosition,
																					const Ats::Core::CSymbol &					objSymbol);

			void enforceMaxOrderSizeBaseCcyLimit(const Ats::Order::COrderFill &	objOrderFill,
																					 const Ats::Core::CSymbol &			objSymbol);

			void enforceMinTradeInterval(const Ats::Order::COrderFill &	objOrderFill,
																	 const Ats::Core::CSymbol &			objSymbol);

			bool isOrderCompliant(Ats::Order::COrder & objOrder);

			bool isConcentrationLimitBreached(const Ats::Core::CSymbol &	objSymbol,
																				Ats::Order::COrder::BuySell	iDirection,
																				unsigned int								iQuantityUSD,
																				unsigned int								iCcyPosLimitUSD) const;

			bool isPnLStopHit() const;

			void resetStop();

			double getDailyDD() const;

			double getDailyPnL() const;

			void setEventProcessor(const Ats::Indicator::CEventProcessor & objEventProcessor);

			void setPortfolioConstraintManager(const Ats::Risk::CPortfolioConstraintManager & objPortfolioConstraintManager);

			void applyRiskMultiplier(double dMultiplier);

			// drawdown control
			double getDD() const;

			double getDDC() const;

			void onFillDDC(const Ats::Order::COrderFill & objOrderFill);

			bool isDDCAdjustmentRequired() const;

			double getDDCMaxSpreadBps() const;

			bool useDDC() const;

			void setDDCParameters(double dNotionalCapitalUSD,
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
														bool	 bUseDDC);

			void revalLastExecution(unsigned int									iSymbolID,
															const Ats::Core::CDateTime &	dtNow,
															double												dMidPx);

		private:
			std::string buildPositionMessage(const Ats::Core::CSymbol & objSymbol) const;
			std::string buildPnLUSDMessage() const;

		protected:
			// instruments the algorithm is interested in trading
			typedef std::vector<Ats::Core::CSymbol>						TradedInstrumentArray;

			// maintains whether each provider is online or not
			typedef boost::unordered_map<unsigned int, bool>	ProviderAvailabilityMap;

			// rolling spread history
			typedef boost::unordered_map<unsigned int, Ats::Indicator::CCircularScalarBuffer>				RollingSpreadHist;
			typedef boost::unordered_map<unsigned int, Ats::Indicator::CCircularScalarBuffer>				RollingSpreadMAHist;
			typedef boost::unordered_map<unsigned int, std::vector<double> >												OrderRevalMap;

			// short term rolling ATR
			typedef boost::unordered_map<unsigned int, Ats::Indicator::CRollingATR>									RollingATR;

			enum { DAYS_PER_WEEK = 7, MINUTES_PER_DAY = 1440 };

			// flags specifying traded instruments
			unsigned int																			m_TradedInstrumentMap[Ats::Core::CConfig::MAX_NUM_SYMBOLS];

			// positions across all symbols for this algorithm
			Ats::Accounting::CPositionManager									m_PositionManager;

			// positions and pnl by currency
			Ats::Risk::CResidualsManager											m_Residuals;

			// algorithm log file
			Ats::Log::CLog																		m_Logger;

			// instruments traded by this algorithm
			TradedInstrumentArray															m_TradedInstruments;

			// provider availability
			ProviderAvailabilityMap														m_ProviderStatus;

			// event processor
			Ats::Indicator::CEventProcessor										m_EventProcessor;

			// constraint manager
			Ats::Risk::CPortfolioConstraintManager						m_PortfolioConstraintManager;

			// drawdown control
			Ats::Risk::CDrawdownControl												m_DrawdownControl;

		private:
			// subscribers interested in receiving messages from the algorithm (namely the engine which will broadcast the message to the GUI)
			typedef std::vector<IAlgorithmSubscriber *>				AlgorithmSubscriberArray;

			AlgorithmSubscriberArray													m_AlgoSubscribers;

			unsigned int																			m_uiAlgoID;
			std::string																				m_strAlgoName;
			bool																							m_bStopped;

			bool																							m_bEventQEnabled;

			// event queue will be processed on a separate thread
			boost::shared_ptr<boost::thread>									m_ptrThread;

#ifdef LOCKFREE
			// algorithm specific event queue (lock free)
			Ats::Event::CEventQueueLockFree<Ats::Event::CEvent::EventID>					m_EventIDQ;					// single queue of event IDs to track sequence in which events were captured
			Ats::Event::CEventQueueLockFree<Ats::OrderBook::CConsolidatedBook>		m_EventOrderBookQ;	// individual queues per event type
			Ats::Event::CEventQueueLockFree<Ats::OrderBook::CBidAsk>							m_EventBidAskQ;
			Ats::Event::CEventQueueLockFree<Ats::OrderBook::CQuote>								m_EventQuoteQ;
			Ats::Event::CEventQueueLockFree<Ats::OrderBook::CTrade>								m_EventTradeQ;
			Ats::Event::CEventQueueLockFree<Ats::Order::COrderStatus>							m_EventOrderStatusQ;
			Ats::Event::CEventQueueLockFree<Ats::Order::COrderFill>								m_EventOrderFillQ;
			Ats::Event::CEventQueueLockFree<Ats::Core::CDateTimeEvent>						m_EventDateTimeQ;
#else
			// algorithm specific event queue (lock based)
			Ats::Event::CEventQueue														m_EventQ;
#endif

			// throttle (millseconds) for consolidated book (level II) updates
			Ats::Core::CDateTime															m_dtLastUpdate[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			unsigned int																			m_iUpdateMSec;
			double																						m_dUpdateIntervalDayFrac;
			double																						m_dDefaultUpdateIntervalDayFrac;
			double																						m_dThrottledUpdateIntervalDayFrac;

			// throttle (milliseconds) for pnl updates
			Ats::Core::CDateTime															m_dtLastUpdatePnL[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			unsigned int																			m_iUpdateMSecPnL;

			OperationalMode																		m_iOperationalMode;

			// algorithm specific risk limits
			int																								m_iMaxPositionBaseCcy[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			int																								m_iMaxOrderSizeBaseCcy[Ats::Core::CConfig::MAX_NUM_SYMBOLS];

			// minimum time interval between trades (risk check to prevent machine gunning)
			Ats::Core::CDateTime															m_dtLastFillTime[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			double																						m_dMinTradeIntervalDayFrac;

			// max daily loss (trailing)
			double																						m_dDailyMaxLoss;
			double																						m_dCurrPnLUSD;
			double																						m_dMaxPnLUSD;
			double																						m_dStartOfDayPnLUSD;

			// PnL by currency
			double																						m_dCurrCcyPnLUSD[Ats::Core::CConfig::MAX_NUM_CCYS];
			double																						m_dMaxCcyPnLUSD[Ats::Core::CConfig::MAX_NUM_CCYS];
			double																						m_dStartOfDayCcyPnLUSD[Ats::Core::CConfig::MAX_NUM_CCYS];

			// PnL by pair
			double																						m_dCurrPairPnLUSD[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			double																						m_dMaxPairPnLUSD[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			double																						m_dStartOfDayPairPnLUSD[Ats::Core::CConfig::MAX_NUM_SYMBOLS];

			// new stop losses (trailing) once profit target hit
			bool																							m_bUseTrailingStop;
			double																						m_dDailyProfitThresholdUSD1;
			double																						m_dProfitLockUSD1;
			double																						m_dDailyProfitThresholdUSD2;
			double																						m_dProfitLockUSD2;

			// set to true if a portfolio (engine) level stop has been triggered
			bool																							m_bPortfolioStopHit;

			// short term rolling ATR
			RollingATR																				m_RollingATR;

			// rolling spread history - rolling n second
			RollingSpreadHist																	m_RollingSpreadHistFast;
			RollingSpreadHist																	m_RollingSpreadHistSlow;

			// spread history over past week
			double																						m_SpreadMAHist[Ats::Core::CConfig::MAX_NUM_SYMBOLS][DAYS_PER_WEEK * MINUTES_PER_DAY];

			// last execution per pair - used to reval flow. assumes trade interval of 30+ secs between successive trades in each pair
			double																						m_dLastExecPx[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			Ats::Core::CDateTime															m_dtLastExecTime[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			double																						m_dLastExecDirection[Ats::Core::CConfig::MAX_NUM_SYMBOLS];
			std::vector<double>																m_RevalIntSecsArray;
			std::vector<double>																m_RevalIntDayFracArray;
			OrderRevalMap																			m_Revals;
		};
	}
}

#include "CAlgorithm.hxx"

#endif

