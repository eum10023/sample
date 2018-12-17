//===============================================================================
// Copyright (c) 2018, Optimal Alpha LLC
//===============================================================================

using namespace Ats::Algorithm;


template <typename T>
inline void CAlgorithm::handleEvent(Ats::Event::CEvent::EventID iEventID) {

	T evt;

	m_EventQ.pop(evt);
	m_EventQ.unlock();

	evt.setEventID(iEventID);

	onEvent(evt);
}


inline Ats::Algorithm::CAlgorithm::OperationalMode CAlgorithm::getOperationalMode() const {

	return m_iOperationalMode;
}


inline void CAlgorithm::setOperationalMode(OperationalMode iOperationalMode) {

	m_iOperationalMode = iOperationalMode;

	Ats::Core::CDateTime dtNow = Ats::Core::CDateTime::now();
	char szBuffer[512];

	switch (m_iOperationalMode) {

		case PASSIVE:
			sprintf(szBuffer, "%llu Change operational mode to PASSIVE\n", dtNow.getCMTime());
			m_Logger.log(szBuffer);
			break;

		case LIQUIDATE:
			sprintf(szBuffer, "%llu Change operational mode to LIQUIDATE\n", dtNow.getCMTime());
			m_Logger.log(szBuffer);
			break;

		case ACTIVE:
			sprintf(szBuffer, "%llu Change operational mode to ACTIVE\n", dtNow.getCMTime());
			m_Logger.log(szBuffer);
			break;
	}
}

