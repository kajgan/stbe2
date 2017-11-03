#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>
#include <linux/dvb/ca.h>
#include <linux/dvb/version.h>
#include <lib/base/eerror.h>
#include <lib/base/init.h>
#include <lib/base/init_num.h>
#include <lib/base/wrappers.h>
#include <lib/base/nconfig.h>
#include <lib/base/cfile.h>
#include <lib/base/e2avahi.h>
#include <lib/dvb/decoder.h>

#include <lib/dvb/rtspstreamserver.h>
#include <lib/dvb/encoder.h>
#include <lib/dvb/db.h>
#include <lib/dvb_ci/dvbci.h>

#define _ADD_PIDS 1
#define _DEL_PIDS 2
#define _PIDS 3

#ifndef SYS_DVBC2
#define SYS_DVBC2 19 // support for DVB-C2 DD
#endif

#ifndef SYS_DVBT2
#define SYS_DVBT2 16
#endif

static int global_stream_id = 0;
int tcp_port = 8554;

static const char *satip_xml =
	"<?xml version=\"1.0\"?>"
	"<root xmlns=\"urn:schemas-upnp-org:device-1-0\" configId=\"0\">"
	"<specVersion><major>1</major><minor>1</minor></specVersion>"
	"<device><deviceType>urn:ses-com:device:SatIPServer:1</deviceType>"
	"<friendlyName>enigma</friendlyName><manufacturer>Gigablue</manufacturer>"
	"<manufacturerURL>http://gigablue.de</manufacturerURL>"
	"<modelDescription>Enigma for Linux</modelDescription><modelName>enigma</modelName>"
	"<modelNumber>1.1</modelNumber><modelURL></modelURL><serialNumber>1</serialNumber><UDN>uuid:11223344-9999-0001-b7ae</UDN>"
	"<iconList>"
	//	"<icon><mimetype>image/png</mimetype><width>48</width><height>48</height><depth>24</depth><url>/sm.png</url></icon>"
	//	"<icon><mimetype>image/png</mimetype><width>120</width><height>120</height><depth>24</depth><url>/lr.png</url></icon>"
	//	"<icon><mimetype>image/jpeg</mimetype><width>48</width><height>48</height><depth>24</depth><url>/sm.jpg</url></icon>"
	//	"<icon><mimetype>image/jpeg</mimetype><width>120</width><height>120</height><depth>24</depth><url>/lr.jpg</url></icon>"
	"</iconList>"
	"<presentationURL>http://%s:%d/</presentationURL>\r\n"
	"<satip:X_SATIPCAP xmlns:satip=\"urn:ses-com:satip\">%s</satip:X_SATIPCAP>"
	"%s"
	"</device></root>";

std::set<eServiceReferenceDVB> processed_sr;

eRTSPStreamClient::eRTSPStreamClient(eRTSPStreamServer *handler, int socket, const std::string remotehost)
	: parent(handler), encoderFd(-1), streamFd(socket), streamThread(NULL), m_remotehost(remotehost)
{
	session_id = 0;
	stream_id = 0;
	eDebug("Client starting %d", streamFd);
	init_rtsp();
}

eRTSPStreamClient::~eRTSPStreamClient()
{
	eDebug("Client terminating %d", streamFd);
	pids.clear();
	update_service_list();
	rsn->stop();
	stop();
	if (streamThread)
	{
		streamThread->stop();
		delete streamThread;
	}
	if (encoderFd >= 0)
	{
		if (eEncoder::getInstance())
			eEncoder::getInstance()->freeEncoder(encoderFd);
	}
	if (streamFd >= 0)
		::close(streamFd);
	eDebug("done disconnecting client, %d", streamFd);
}

void eRTSPStreamClient::init_rtsp()
{
	running = false;
	fp = NULL;
	freq = 0;
	pol = -1;
	sys = 0;
	first = true;
	fe = 0;
	src = 0;
	buf_size = 0;
	m_serviceref = "";
	mr = NULL;
	clear_previous_channel = 0;
	m_tuned = 0;
	m_channel = NULL;
	tune_completed = 0;
	m_record_no_pids = 1;
	n_service_list = 0;
	time_addsr = 0;
	transponder_id = 0;
	proto = PROTO_RTSP_TCP;
}

void eRTSPStreamClient::start()
{
	rsn = eSocketNotifier::create(eApp, streamFd, eSocketNotifier::Read);
	CONNECT(rsn->activated, eRTSPStreamClient::notifier);
}

char *strip(char *s) // strip spaces from the front of a string
{
	if (s < (char *)1000)
		return NULL;

	while (*s && *s == ' ')
		s++;
	return s;
}

int split(char **rv, char *s, int lrv, char sep)
{
	int i = 0, j = 0;

	if (!s)
		return 0;
	for (i = 0; s[i] && s[i] == sep && s[i] < 32; i++)
		;

	rv[j++] = &s[i];
	//      LOG("start %d %d\n",i,j);
	while (j < lrv)
	{
		if (s[i] == 0 || s[i + 1] == 0)
			break;
		if (s[i] == sep || s[i] < 33)
		{
			s[i] = 0;
			if (s[i + 1] != sep && s[i + 1] > 32)
				rv[j++] = &s[i + 1];
		}
		else if (s[i] < 14)
			s[i] = 0;
		//              LOG("i=%d j=%d %d %c \n",i,j,s[i],s[i]);
		i++;
	}
	if (s[i] == sep)
		s[i] = 0;
	rv[j] = NULL;
	return j;
}

#define LR(s)                         \
	{                                 \
		LOG("map_int returns %d", s); \
		return s;                     \
	}
int map_intd(char *s, char **v, int dv)
{
	int i, n = dv;

	if (s == NULL)
	{
		eDebug("map_int: s=>NULL, v=%p, %s %s", v, v ? v[0] : "NULL", v ? v[1] : "NULL");
		return dv;
	}

	s = strip(s);

	if (!*s)
	{
		eDebug("map_int: s is empty");
		return dv;
	}
	if (v == NULL)
	{
		if (s[0] != '+' && s[0] != '-' && (s[0] < '0' || s[0] > '9'))
		{
			eDebug("map_int: s not a number: %s, v=%p, %s %s", s, v, v ? v[0] : "NULL", v ? v[1] : "NULL");
			return dv;
		}
		return atoi(s);
	}
	for (i = 0; v[i]; i++)
		if (!strncasecmp(s, v[i], strlen(v[i])))
			n = i;
	return n;
}

void eRTSPStreamClient::getFontends(int &dvbt, int &dvbt2, int &dvbs2, int &dvbc, int &dvbc2)
{
	ePtr<eDVBResourceManager> m_res_mgr;
	eDVBResourceManager::getInstance(m_res_mgr);
	eSmartPtrList<eDVBRegisteredFrontend> &frontends = m_res_mgr->m_frontend;
	dvbt = 0;
	dvbt2 = 0;
	dvbs2 = 0;
	dvbc = 0;
	dvbc2 = 0;
	for (eSmartPtrList<eDVBRegisteredFrontend>::iterator it(frontends.begin()); it != frontends.end(); ++it)
	{
		if (it->m_frontend->supportsDeliverySystem((fe_delivery_system_t)SYS_DVBS2, false))
			dvbs2++;
		if (it->m_frontend->supportsDeliverySystem((fe_delivery_system_t)SYS_DVBC_ANNEX_A, false))
			dvbc++;
		if (it->m_frontend->supportsDeliverySystem((fe_delivery_system_t)SYS_DVBC2, false))
			dvbc2++;
		if (it->m_frontend->supportsDeliverySystem((fe_delivery_system_t)SYS_DVBT2, false))
			dvbt2++;
		if (it->m_frontend->supportsDeliverySystem((fe_delivery_system_t)SYS_DVBT, false))
			dvbt++;
	}
	dvbt = dvbt - dvbt2;
	dvbc = dvbc - dvbc2;

	if (dvbt < 0)
		dvbt = 0;
	if (dvbc < 0)
		dvbc = 0;

	eDebug("Identified: dvbt %d, dvbt2 %d, dvbs2 %d, dvbc %d, dvbc2 %d", dvbt, dvbt2, dvbs2, dvbc, dvbc2);
}

int eRTSPStreamClient::getOrbitalPosition(int fe, int src)
{
	int op = 0;

	int current_src = 0;
	eDVBSatelliteEquipmentControl *m_sec = eDVBSatelliteEquipmentControl::getInstance();

	if (src < 0 || src >= (int)sizeof(m_sec->m_lnbs))
	{
		eDebug("invalid src %d", src);
		return 0;
	}
	for (int i = 0; i < 144; i++)
	{
		eDVBSatelliteLNBParameters &lnb_param = m_sec->m_lnbs[i];
		int op1 = 0;
		if (lnb_param.m_satellites.size() > 0)
		{
			for (std::map<int, eDVBSatelliteSwitchParameters>::iterator sit = lnb_param.m_satellites.begin(); sit != lnb_param.m_satellites.end(); ++sit)
				op1 = sit->first;
			eDebug("LNB OP %d", op1);
		}
		if ((fe >= 0) && (lnb_param.m_slot_mask & (1 << fe)))
		{
			current_src++;
			op = op1;
		}
		eDebug("%d: orbital position %d, m_slot_mask %d, lnNum %d, old OP %d", i, op1, lnb_param.m_slot_mask, lnb_param.m_satellites.size(), lnb_param.SatCR_idx); //lnb_param.LNBNum, lnb_param.old_orbital_position);
		if (current_src >= src)
		{
			eDebug("orbital position matched to %d", op1);
			op = op1;
			break;
		}
	}
	//		op = sit->first;
	if (op)
		eDebug("found orbital position %d for src %d", op, src);
	else
		eDebug("orbital position not found for src %d", src);
	return op;
}

std::string eRTSPStreamClient::searchServiceRef(int sys, int freq, int pol, int orbital_position, int sid)
{
	eDebug("start %s", __FUNCTION__);
	eDVBFrontendParametersSatellite sat1;
	eDVBFrontendParametersTerrestrial ter1;
	eDVBFrontendParametersCable cab1;
	n_service_list = 0;
	int found = 0;
	memset(&sat, 0, sizeof(sat));
	memset(&ter, 0, sizeof(ter));
	memset(&cab, 0, sizeof(cab));
	const eServiceReferenceDVB *srvc = NULL;
	for (std::map<eServiceReferenceDVB, ePtr<eDVBService>>::iterator i(m_dvbdb->m_services.begin());
		 i != m_dvbdb->m_services.end(); ++i)
	{
		found = 0;
		unsigned int flags = 0;
		eDVBChannelID chid;
		ePtr<iDVBFrontendParameters> p;
		const eServiceReferenceDVB &s = i->first;
		s.getChannelID(chid);
		m_dvbdb->getChannelFrontendData(chid, p);
		flags = i->second->m_flags;
		if (!p)
			continue;
		if (!p->getDVBS(sat1) && ((sys == SYS_DVBS) || (sys == SYS_DVBS2)))
		{
			//			eDebug("freq = %d, sat.freq %d, OP %d, SOP %d, pol %d pol %d", freq, sat1.frequency, orbital_position, sat1.orbital_position, pol, sat1.polarisation);
			if ((abs(sat1.frequency - freq) < 2000) && sat1.polarisation == pol && orbital_position == sat1.orbital_position)
			{
				sat = sat1;
				eDebug("Adding %s to the list for frequency %d (%s) f:%x", s.toString().c_str(), sat.frequency, i->second->m_service_name.c_str(), flags);
				service_list[n_service_list++] = s;
				srvc = &i->first;
				found = 1;
			}
		}

		if (!p->getDVBT(ter1) && ((sys == SYS_DVBT) || (sys == SYS_DVBT2)))
		{
			if ((abs(ter1.frequency / 1000 - freq) < 2000))
			{
				ter = ter1;
				eDebug("Adding %s to the list (%s) f:%x", s.toString().c_str(), i->second->m_service_name.c_str(), flags);
				service_list[n_service_list++] = s;
				srvc = &i->first;
				found = 1;
			}
		}

		if (!p->getDVBC(cab1) && ((sys == SYS_DVBC_ANNEX_A) || (sys == SYS_DVBC2)))
		{
			if ((abs(cab1.frequency - freq) < 2000))
			{
				cab = cab1;
				eDebug("Adding %s to the list (%s) f:%x", s.toString().c_str(), i->second->m_service_name.c_str(), flags);
				service_list[n_service_list++] = s;
				srvc = &i->first;
				found = 1;
			}
		}

		std::set<eServiceReferenceDVB>::iterator it = processed_sr.find(s);
		int not_available = (it == processed_sr.end());
		if (found)
		{
			if (addCachedPids(i->second, s))
			{
				if (not_available)
				{
					eDebug("SR %s does not have cached pids", s.toString().c_str());
					not_cached_sr.insert(s);
				}
				else
					eDebug("SR %s was already attempted and does not have cached pids, not adding", s.toString().c_str());
			}
		}
	}
	if (srvc)
	{
		//		return srvc->toString();
		const eServiceReferenceDVB srv(srvc->getDVBNamespace(), srvc->getTransportStreamID(), srvc->getOriginalNetworkID(), eServiceID(sid), srvc->getServiceType(), srvc->getSourceID());
		return srv.toString();
	}
	eDebug("end %s", __FUNCTION__);
	return "";
}

void eRTSPStreamClient::process_pids(int op, char *pid_str)
{
	int n = ::strlen(pid_str);
	int la;
	char buf[n + 10];
	char *arg[128];
	memset(buf, 0, n + 10);
	char *sep = ::strchr(buf, '?');
	if (sep)
		*sep = 0;
	strncpy(buf, pid_str, n);

	if (op == _PIDS)
		pids.clear();

	if (buf_size == 0)
		pids.insert(301);
	else
		pids.erase(301);

	eDebug("%s: operation %d, pid_str %s (len pids: %d)", __FUNCTION__, op, pid_str, pids.size());
	if (op == _PIDS && !::strcmp(buf, "all"))
	{
		pids.insert(8192);
		update_service_list();
		return;
	}

	la = split(arg, buf, 128, ',');
	for (int i = 0; i < la; i++)
	{
		int p = map_intd(arg[i], NULL, -1);
		if (p < 0 && p > 8191)
			continue;
		if (op == _PIDS || op == _ADD_PIDS)
			add_pid(p);
		if (op == _DEL_PIDS)
			del_pid(p);
	}
	update_service_list();
}

const char *fe_delsys[] =
	{"undefined", "dvbc", "dvbcb", "dvbt", "dss", "dvbs", "dvbs2", "dvbh", "isdbt",
	 "isdbs", "isdbc", "atsc", "atscmh", "dmbth", "cmmb", "dab", "dvbt2",
	 "turbo", "dvbcc", "dvbc2",
	 NULL};
const char *fe_pol[] = {"h", "v", "l", "r", NULL};
int eRTSPStreamClient::satip2enigma(std::string satipstr)
{
	int freq1 = 0, pol1 = -1, sys1 = 0, sid = 0;
	char *sep = NULL, *addpids = NULL, *delpids = NULL, *pids = NULL;
	int n = satipstr.size();
	char buf[n + 10];
	int do_tune = 0;
	eDVBResourceManager::getInstance(m_mgr);
	/* als Primary datenbank setzen */
	m_dvbdb = eDVBDB::getInstance();
	eDebug("Start %s", __FUNCTION__);
	/* testtransponder adden */
	::memset(buf, 0, n + 10);
	::strncpy(buf, satipstr.c_str(), n);

	sep = (char *)::strstr(buf, (char *)"fe=");
	if (sep)
		fe = map_intd(buf + 3, NULL, 0) - 1;

	sep = (char *)::strstr(buf, (char *)"src=");
	if (sep)
		src = map_intd(sep + 4, NULL, 0) - 1;

	sep = (char *)::strstr(buf, (char *)"freq=");
	if (sep)
		freq1 = map_intd(sep + 5, NULL, 0) * 1000;

	sep = (char *)::strstr(buf, (char *)"msys=");
	if (sep)
		sys1 = map_intd(sep + 5, (char **)fe_delsys, 0);

	sep = (char *)::strstr(buf, (char *)"pol=");
	if (sep)
		pol1 = map_intd(sep + 4, (char **)fe_pol, -1);

	sep = (char *)::strstr(buf, (char *)"sid=");
	if (sep)
		sid = map_intd(sep + 4, NULL, 0);

	if (freq && freq1 && (freq != freq1))
		do_tune = 1;

	if ((pol != -1) && (pol1 != -1) && (pol != pol1))
		do_tune = 1;

	eDebug("initial values, freq %d, pol %d, sys %d, old freq = %d, tune %d", freq1, pol1, sys1, freq, do_tune);

	if (do_tune)
	{
		eDebug("Tuning multiple transponders not supported, state %d", m_state);
		clear_previous_channel = 1;
		this->pids.clear();
		n_service_list = 0;
		//	m_state = stateIdle;
		update_pids();
		return -1;
		stop();
		eDebug("free service handler, state %d", m_state);
		m_service_handler.free();
		eDebug("done freeing service handler");
		m_mgr->removeChannel(m_channel);
		m_channel->stop();
		m_record = NULL;
		init_rtsp();
		clear_previous_channel = 1;
		//return -1;
	}
	if (freq1)
		freq = freq1;
	if (pol1 != -1)
		pol = pol1;
	if (sys1)
		sys = sys1;

	if ((addpids = (char *)::strstr(buf, (char *)"addpids=")))
		addpids += 8;

	if ((delpids = (char *)::strstr(buf, (char *)"delpids=")))
		delpids += 8;

	if ((pids = (char *)::strstr(buf, (char *)"pids=")))
		pids += 5;

	if (addpids)
		process_pids(_ADD_PIDS, addpids);

	if (delpids)
		process_pids(_DEL_PIDS, delpids);

	if (pids && !addpids && !delpids)
		process_pids(_PIDS, pids);

	std::string sref = "";
	int op = 0;
	if (sys == SYS_DVBS || sys == SYS_DVBS2)
		op = getOrbitalPosition(fe, src);
	eDebug("tunning to %d, pol %d, sys %d", freq, pol, sys);
	if (freq1 && ("" != (sref = searchServiceRef(sys, freq, pol, op, sid)))) // 0 - horizontal, 1 - vertical , 2 ->left, 3->Right
	{
		eDebug("Using service ref %s, state %d %d", sref.c_str(), m_state, stateIdle);
		m_serviceref = sref;
		running = true;
		if (!stream_id)
		{
			stream_id = __sync_add_and_fetch(&global_stream_id, 1);
			session_id = random();
		}
	}
	else
		eDebug("no service ref used");
	return 0;
}

int eRTSPStreamClient::addCachedPids(ePtr<eDVBService> service, eServiceReferenceDVB s)
{
	int found = 0;
	for (int x = 0; x < 5; ++x)
	{
		int entry = service->getCacheEntry((eDVBService::cacheID)x);
		if (entry != -1)
		{
			eDebug("Found cached pid %d [%x]", entry, entry);
			pid_sr[entry] = s;
			found = 1;
		}
	}
	return !found;
}

int64_t getTick();

void eRTSPStreamClient::update_service_list()
{
	eServiceReferenceDVB sr;
	//	uint64_t now = getTick();
	std::set<eServiceReferenceDVB> new_sr, obsolete_sr, delete_cached;
	if (clear_previous_channel == 1)
		return;

	for (std::map<eServiceReferenceDVB, eDVBServicePMTHandler *>::iterator it = active_services.begin(); it != active_services.end(); it++)
		obsolete_sr.insert(it->first);

	std::set<eServiceReferenceDVB>::iterator it;

	// check if we have cached vpid, if no keep the PMTHandler added until we do

	for (it = not_cached_sr.begin(); it != not_cached_sr.end(); it++)
	{
		eServiceReferenceDVB ref = it->getParentServiceReference();
		ePtr<eDVBService> service;

		if (!ref.valid())
			ref = *it;

		if (eDVBDB::getInstance()->getService(ref, service))
		{
			eDebug("Service reference %s not found in the DB", ref.toString().c_str());
			continue;
		}
		eDVBServicePMTHandler::program p;
		std::map<eServiceReferenceDVB, eDVBServicePMTHandler *>::iterator it3 = active_services.find(*it);
		if (it3 != active_services.end())
		{
			eDebug("getting program info for %s", it->toString().c_str());
			if (it3->second->getProgramInfo(p))
				eDebug("getting program info failed for %s", it->toString().c_str());
			else
			{ // cache the pid, next time we won't be needed to create the pmt handler
				int vpid = -1, vpidtype = -1, apid = -1, apidtype = -1;
				for (std::vector<eDVBServicePMTHandler::videoStream>::const_iterator
						 i(p.videoStreams.begin());
					 i != p.videoStreams.end(); ++i)
				{
					eDebug("Identified vpid %d, type %d", i->pid, i->type);
					if (vpid == -1)
					{
						vpid = i->pid;
						vpidtype = i->type;
					}
				}
				for (std::vector<eDVBServicePMTHandler::audioStream>::const_iterator
						 i(p.audioStreams.begin());
					 i != p.audioStreams.end(); ++i)
				{
					eDebug("Identified apid %d, type %d", i->pid, i->type); // we do not really need the apids, for video channels
					if (apid == -1)
					{
						apid = i->pid;
						apidtype = i->type;
					}
				}

				if (apid != -1) // keep code consistent with streamdvb.cpp: eDVBServicePlay::selectAudioStream
				{
					eDebug("Adding apid %d for service %s", apid, ref.toString().c_str());
					service->setCacheEntry(eDVBService::cMPEGAPID, apidtype == eDVBAudio::aMPEG ? apid : -1);
					service->setCacheEntry(eDVBService::cAC3PID, apidtype == eDVBAudio::aAC3 ? apid : -1);
					service->setCacheEntry(eDVBService::cDDPPID, apidtype == eDVBAudio::aDDP ? apid : -1);
					service->setCacheEntry(eDVBService::cAACHEAPID, apidtype == eDVBAudio::aAACHE ? apid : -1);
					service->setCacheEntry(eDVBService::cAACAPID, apidtype == eDVBAudio::aAAC ? apid : -1);
					if (vpid == -1)
						continue;
				}
				if (vpid != -1) // keep code consistent with streamdvb.cpp: eDVBServicePlay::updateDecoder
				{
					eDebug("Adding vpid %d for service %s", vpid, ref.toString().c_str());
					service->setCacheEntry(eDVBService::cVPID, vpid);
					service->setCacheEntry(eDVBService::cVTYPE, vpidtype == eDVBVideo::MPEG2 ? -1 : vpidtype);
					pid_sr[vpid] = ref;
					delete_cached.insert(ref);
					continue;
				}
			}
		}
		if (m_record && !time_addsr)
			time_addsr = getTick();
		if (time_addsr && (getTick() - time_addsr > 2000))
		{
			eDebug("deleting SR %s because it timed out", ref.toString().c_str());
			delete_cached.insert(ref);
			processed_sr.insert(ref);
			continue;
		}
		if (pids.size() > 0)
		{
			eDebug("Service Ref %s still does not have cached pids, adding", ref.toString().c_str());
			new_sr.insert(ref);
			obsolete_sr.erase(ref);
		}
	}

	for (it = delete_cached.begin(); it != delete_cached.end(); it++)
		not_cached_sr.erase(*it);

	for (std::set<int>::iterator it = pids.begin(); it != pids.end(); it++)
	{
		int pid = *it;
		std::map<int, eServiceReferenceDVB>::iterator it2 = pid_sr.find(pid);
		if (it2 != pid_sr.end())
		{
			sr = it2->second;
			obsolete_sr.erase(sr);
			std::map<eServiceReferenceDVB, eDVBServicePMTHandler *>::iterator it3 = active_services.find(sr);
			if (it3 == active_services.end())
			{
				eDebug("Found SR %s for pid %d", sr.toString().c_str(), pid);
				new_sr.insert(sr);
			}
			//			else eDebug("SR %s already in the list for pid %d", sr.toString().c_str(), pid );
			buf_size = 1;
		}
	}

	if (m_record || (pids.size() == 0))
	{
		for (std::set<eServiceReferenceDVB>::iterator it = obsolete_sr.begin(); it != obsolete_sr.end(); it++)
		{
			eDVBServicePMTHandler *pmth = active_services[*it];
			eDebug("Deleting service reference %s", it->toString().c_str());
			delete (pmth);
			active_services.erase(*it);
		}

		for (std::set<eServiceReferenceDVB>::iterator it = new_sr.begin(); it != new_sr.end(); it++)
		{

			//			eDVBChannelID chid;//(dvbnamespace,transport_stream_id,original_network_id);
			std::map<eServiceReferenceDVB, eDVBServicePMTHandler *>::iterator it3 = active_services.find(*it);
			if (it3 == active_services.end())
			{
				eServiceReferenceDVB m_ref = *it;
				eDVBServicePMTHandler *pmth = new eDVBServicePMTHandler();
				eDebug("New PMT Handler for SR %s", it->toString().c_str());
				pmth->tune(m_ref, 0, 0, 0, NULL, eDVBServicePMTHandler::scrambled_streamserver, 1);
				active_services[*it] = pmth;
				ePtr<iDVBDemux> m_demux;
				if (!pmth->getDataDemux(m_demux))
				{
					uint8_t d, a, od = -1, oa = -1;
					m_demux->getCADemuxID(d);
					m_demux->getCAAdapterID(a);
					if (!m_service_handler.getDataDemux(m_demux))
					{
						m_demux->getCADemuxID(od);
						m_demux->getCAAdapterID(oa);
					}
					eDebug("got demux %d adapter %d, original demux: %d adapter %d ", d, a, od, oa);
				}

				buf_size = 1;

				//				CONNECT(pmth->serviceEvent, eRTSPStreamClient::serviceEvent);
			}
			//			else eDebug("SR %s already in the list %s", it->toString().c_str(), it3->first.toString().c_str());
		}
	}
}

void eRTSPStreamClient::add_pid(int p)
{
	if (p < 0 || p > 8192)
		return;
	std::set<int>::iterator findIter = std::find(pids.begin(), pids.end(), p);
	if (findIter != pids.end())
	{
		eDebug("pid already in the list %d", p);
		return;
	}
	pids.insert(p);
	update_pids();
	//	update_service_list();
}

void eRTSPStreamClient::update_pids()
{
	eDebug("update pids called: %d pids in the queue", pids.size());
	std::set<int>::iterator it = pids.find(0);
	if (pids.size() > 0 && it == pids.end())
		pids.insert(0);
	if (running && m_record)
		recordPids(pids, -1, -1, iDVBTSRecorder::none);
}

void eRTSPStreamClient::del_pid(int p)
{
	std::set<int>::iterator findIter = std::find(pids.begin(), pids.end(), p);
	if (findIter == pids.end())
	{
		eDebug("pid already removed from the list %d", p);
		return;
	}

	pids.erase(p);
	update_pids();

	//	update_service_list();
}

/*
   eServiceReferenceDVB *eRTSPStreamClient::getServiceforPid(int p)
   {
   eServiceReferenceDVB *m_ref = NULL;
   int sid = 0;
   if(first && (m_service_handler.pid_sid.size() > 0))
   {
   first = false;
   for(std::map<int, int>::iterator search = m_service_handler.pid_sid.begin(); search != m_service_handler.pid_sid.end(); search++)
   {
   eDebug("PMT pid %d sid %d", search->first, search->second);
   }
   }
   std::map<int, int>::iterator search = m_service_handler.pid_sid.find(p);
   if(search != m_service_handler.pid_sid.end())
   {
   sid = search->second;
   for(int i = 0; i<n_service_list; i++)
   {
   if(service_list[i].getServiceID() == sid)
    m_ref = &service_list[i];
   }
   }
   if(m_ref == NULL)
   {
   eDebug("service ref for pid %d, sid %d not found", p, sid);
   return NULL;
   }
   eDebug("pid %d returned service ref %s", p, m_ref->toString().c_str());
   return m_ref;
   }
 */

const char *event_desc[] = {"NoResources", "TuneFailed", "NoPAT", "NoPATEntry", "NoPMT", "NewProgramInfo", "Tuned", "PreStart", "SOF", "EOF", "Misconfiguration", "HBBTVInfo", "Stopped"};

int eRTSPStreamClient::set_demux_buffer(int size)
{
	if (!m_record)
		return -1;
	return 0;
	int rv;
	eDVBTSRecorder *rec = (eDVBTSRecorder *)(iDVBTSRecorder *)m_record;

	rv = rec->setBufferSize(size);
	eDebug("Set DEMUX BUFFER TO %d, returned %d", size, rv);
	return rv;
}

void eRTSPStreamClient::eventUpdate(int event)
{
	if (event >= 0 && event <= sizeof(event_desc))
		eDebug("eventUpdate %s", event_desc[event]);
	else
		eDebug("eventUpdate %d", event);

	if (event == eDVBServicePMTHandler::eventNoResources && m_mgr)
	{
		ePtr<iDVBFrontend> frontend;
		eDebug("No available tunners");
		for (std::list<eDVBResourceManager::active_channel>::iterator i(m_mgr->m_active_channels.begin()); i != m_mgr->m_active_channels.end(); ++i)
		{
			i->m_channel->getFrontend(frontend);
			eDVBFrontend *f = (eDVBFrontend *)(iDVBFrontend *)frontend;
			if (f)
				eDebug("Adapter %d slot %d frequency %d", f->getDVBID(), f->getSlotID(), frontend->readFrontendData(iFrontendInformation_ENUMS::frequency));
		}
	}
	if (event == eDVBServicePMTHandler::eventNoPMT || event == eDVBServicePMTHandler::eventNoPMT || event == eDVBServicePMTHandler::eventNoPATEntry)
		clear_previous_channel = 0;

	if (running && m_record)
	{
		mr = (eDVBRecordFileThread *)((eDVBTSRecorder *)(iDVBTSRecorder *)m_record)->m_thread;
		if (mr->getProtocol() != proto)
		{
			eDebug("Setting protocol %d", proto);
			mr->setProtocol(proto);
			mr->setSession(session_id, stream_id);
			update_pids();
		}
	}

	update_service_list();
}

char *
get_current_timestamp(void)
{
	static char date_str[200];
	time_t date;
	struct tm *t;
	const char *day[] =
		{"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	const char *month[] =
		{"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
		 "Nov", "Dec"};
	time(&date);
	t = gmtime(&date);
	if (!t)
		return (char *)"Fri, Sat Jan 1 00:00:20 2000 GMT";
	snprintf(date_str, sizeof(date_str), "%s, %s %d %02d:%02d:%02d %d GMT",
			 day[t->tm_wday], month[t->tm_mon], t->tm_mday, t->tm_hour,
			 t->tm_min, t->tm_sec, t->tm_year + 1900);
	return date_str;
}

char public_str[] = "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN";
const char *app_name = "enigma_minisatip";
const char *version = "0.1";

void eRTSPStreamClient::http_response(int sock, int rc, char *ah, char *desc, int cseq, int lr)
{
	char *desc1;
	char server[50];
	const char *reply =
		"%s/1.0 %d %s\r\nDate: %s%s%s\r\n%s%s\r\nContent-Length: %d\r\n\r\n%s";
	const char *reply0 = "%s/1.0 %d %s\r\nDate: %s%s%s\r\n%s%s\r\n\r\n";
	char *d;
	char *proto;
	char sess_id[100], scseq[100];

	proto = (char *)"RTSP";

	if (!ah || !ah[0])
		ah = public_str;
	if (!desc)
		desc = (char *)"";
	if (rc == 200)
		d = (char *)"OK";
	else if (rc == 400)
		d = (char *)"Bad Request";
	else if (rc == 403)
		d = (char *)"Forbidden";
	else if (rc == 404)
		d = (char *)"Not Found";
	else if (rc == 500)
		d = (char *)"Internal Server Error";
	else if (rc == 501)
		d = (char *)"Not Implemented";
	else if (rc == 405)
		d = (char *)"Method Not Allowed";
	else if (rc == 454)
		d = (char *)"Session Not Found";
	else
	{
		d = (char *)"Service Unavailable";
		rc = 503;
	}
	char resp[10000];
	desc1 = desc;
	resp[sizeof(resp) - 1] = 0;
	if (!lr)
		lr = strlen(desc);

	sess_id[0] = 0;
	scseq[0] = 0;
	server[0] = 0;

	sprintf(server, "\r\nServer: %s/%s", app_name, version);

	if (session_id && ah && !strstr(ah, "Session") && rc != 454)
		sprintf(sess_id, "\r\nSession: %010d", session_id);
	if (cseq > 0)
		sprintf(scseq, "\r\nCseq: %d", cseq);

	if (lr > 0)
		snprintf(resp, sizeof(resp) - 1, reply, proto, rc, d, get_current_timestamp(), sess_id, scseq, ah, server, lr, desc1);
	else
		snprintf(resp, sizeof(resp) - 1, reply0, proto, rc, d, get_current_timestamp(), sess_id, scseq, ah, server);
	eDebug("reply to %d, mr %p,len %d: %s", sock, mr, strlen(resp), resp);
	if (mr)
	{
		mr->pushReply(resp, strlen(resp));
	}
	else
	{
		struct timespec tv, rem;
		tv.tv_sec = 0;
		tv.tv_nsec = 5000000;
		int times = 20;
		int pos = 0;
		int len = strlen(resp);
		int rb = 0;
		while (pos < len)
		{
			rb = send(sock, resp + pos, len - pos, MSG_NOSIGNAL);
			if (rb > 0)
				pos += rb;
			if (pos == len)
				break;
			if (rb == -1 && (errno != EAGAIN && errno != EWOULDBLOCK))
				break;
			if (rb == 0)
				break;
			eDebug("partial write %d out of %d for socket %d", pos, len, sock);
			nanosleep(&tv, &rem);
			if (times-- < 0)
				break;
		}
		if (pos < len)
			eDebug("error writing %d out of %d to socket %d, errno: %d", pos, len, sock, errno);
		eDebug("wrote successfully %d", len);
	}
}

const char *fe_inversion[] =
	{"off", "on", "", // auto
	 NULL};
const char *fe_rolloff[] =
	{"0.35", "0.20", "0.25", "", //auto
	 NULL};
const char *fe_modulation_sat[] =
	{"", "qpsk", "8psk", "16qam", "16apsk", "32apsk", "dqpsk",
	 NULL};
const char *fe_modulation_ter[] =
	{"qpsk", "16qam", "64qam", "", "256qam",
	 NULL};
const char *fe_modulation_cab[] =
	{"", "16qam", "32qam", "64qam", "128qam", "256qam",
	 NULL};

const char *fe_pilot[] =
	{"on", "off", "", //auto
	 NULL};
const char *fe_fec[] =
	{"", "12", "23", "34", "56", "78", "89", "35", "45", "910", "67", "25", "none",
	 NULL};

const char *fe_bw[] =
	{"8", "7", "6", " ", //auto
	 NULL};

const char *fe_tmode[] =
	{"2k", "8k", "", //auto
	 "4k", "1k", "16k", "32k", "c1", "c3780",
	 NULL};
const char *fe_gi[] =
	{"132", "116", "18", "14", "", // auto
	 "1128", "19128", "19256", "pn420", "pn595", "pn945",
	 NULL};

void eRTSPStreamClient::describe_frontend(char *buf, int max_len)
{
	int strength = 0, status = 0, snr = 0;
	int len = 0;
	m_channel = NULL;
	m_channel = (eDVBChannel *)(iDVBChannel *)m_service_handler.m_channel;
	ePtr<iDVBFrontend> frontend;
	if (m_channel)
	{
		m_channel->getFrontend(frontend);
		if (frontend)
		{
			snr = frontend->readFrontendData(iFrontendInformation_ENUMS::signalQualitydB);
			strength = frontend->readFrontendData(iFrontendInformation_ENUMS::signalPower);
			status = ((frontend->readFrontendData(iFrontendInformation_ENUMS::frontendStatus) & FE_HAS_LOCK) > 0);
			if (snr > 65535)
				snr = snr >> 28;
			else
				snr = snr >> 12;
			strength = strength >> 8;
			eDVBFrontend *f = (eDVBFrontend *)(iDVBFrontend *)frontend;
			eDebug("%s: adapter %d slot %d frequency %d", __FUNCTION__, f->getDVBID(), f->getSlotID(), frontend->readFrontendData(iFrontendInformation_ENUMS::frequency));
		}
	}
	if (sys == 0)
		len += snprintf(buf + len, max_len - len, "ver=1.0;src=1;tuner=%d,0,0,0,0,,,,,,,;pids=", fe + 1);
	else if (sys == SYS_DVBS || sys == SYS_DVBS2)
		len +=
			snprintf(buf + len, max_len - len,
					 "ver=1.0;src=%d;tuner=%d,%d,%d,%d,%d,%s,%s,%s,%s,%s,%d,%s;pids=",
					 src + 1, fe + 1, strength, status, snr,
					 sat.frequency / 1000, fe_pol[sat.polarisation],
					 fe_modulation_sat[sat.modulation], fe_pilot[sat.pilot],
					 fe_rolloff[sat.rolloff], fe_delsys[sys], sat.symbol_rate / 1000,
					 fe_fec[sat.fec]);
	else if (sys == SYS_DVBT || sys == SYS_DVBT2)
		len +=
			snprintf(buf + len, max_len - len,
					 "ver=1.1;tuner=%d,%d,%d,%d,%.2f,%d,%s,%s,%s,%s,%s,%d,%d,%d;pids=",
					 fe + 1, strength, status, snr,
					 (double)ter.frequency / 1000000.0, ter.bandwidth / 1000000, fe_delsys[sys],
					 fe_tmode[ter.transmission_mode], fe_modulation_ter[ter.modulation],
					 fe_gi[ter.guard_interval], "", ter.plp_id, 0, 0);
	else
		len +=
			snprintf(buf + len, max_len - len,
					 "ver=1.2;tuner=%d,%d,%d,%d,%.2f,8,%s,%s,%d,%d,%d,%d,%d;pids=",
					 fe + 1, strength, status, snr,
					 (double)cab.frequency / 1000.0, fe_delsys[sys],
					 fe_modulation_cab[cab.modulation], cab.symbol_rate, 0, 0,
					 0, cab.inversion);

	int pid = -1;
	for (std::set<int>::iterator it = pids.begin(); it != pids.end(); it++)
	{
		pid = *it;
		len += snprintf(buf + len, max_len - len, "%d,", pid);
	}
	if (pid != -1)
		buf[len - 1] = 0;
	eDebug("describe_frontend => %s", buf);
}

void eRTSPStreamClient::notifier(int what)
{
	if (!(what & eSocketNotifier::Read))
		return;

	ePtr<eRTSPStreamClient> ref = this;
	char tmpbuf[4096], buf[8192];
	int len;
	int rlen;
	if ((len = singleRead(streamFd, tmpbuf, sizeof(tmpbuf))) <= 0)
	{
		eDebug("error on reading from socket %d", streamFd);
		rsn->stop();
		stop();
		parent->connectionLost(this);
		return;
	}

	tmpbuf[len] = 0;
	eDebug("rtsp read\n%s", tmpbuf);
	request.append(tmpbuf, len);
	memset(buf, 0, sizeof(buf));
	rlen = request.size();
	if (rlen > (int)sizeof(buf))
		rlen = sizeof(buf);
	strncpy(buf, request.c_str(), rlen);
	//	for(int i = 0; i < strlen(buf); i++)
	//		eDebugNoNewLine("%02X ", buf[i]); eDebug("\n");
	if ((request.find("\n\n") == std::string::npos) && (request.find("\r\n\r\n") == std::string::npos))
	{
		eDebug("New Line not found, continuing to read");
		return;
	}
	int cseq = 0;
	char *sep = ::strcasestr(buf, (char *)"cseq:");

	if (sep)
		cseq = map_intd(sep + 5, NULL, 0);

	int pos1 = request.find(' ', 0);
	int pos = request.find(' ', pos1 + 1);
	std::string url = urlDecode(request.substr(pos1, pos - pos1));
	eDebug("URL = %s", url.c_str());
	eDebug("method = /%s/", request.substr(0, pos1).c_str());
	if ((request.substr(0, 5) != "GET /") && ((request.find("?") != std::string::npos)) && ((request.find("freq=") != std::string::npos) || (request.find("pids=") != std::string::npos)))
	{
		if (satip2enigma(url))
		{
			char error[100];
			snprintf(error, sizeof(error), "Error: Tuning multiple transponders not supprted");
			http_response(streamFd, 404, error, NULL, cseq, 0);
			goto done;
		}
	}

	if ((request.substr(0, 8) == "OPTIONS "))
	{
		http_response(streamFd, 200, public_str, NULL, cseq, 0);
		update_service_list();
		goto done;
	}

	if ((request.substr(0, 9) == "DESCRIBE "))
	{
		char sbuf[1000];
		char buf[100];
		char tp[100];
		int pos = 0;
		pos = request.find('?', 10);
		if (pos == (int)std::string::npos)
			pos = request.find(' ', 10);

		describe_frontend(tp, sizeof(tp));
		snprintf(sbuf, sizeof(sbuf),
				 "m=video 0 RTP/AVP 33\r\nc=IN IP4 0.0.0.0\r\na=control:stream=%d\r\na=fmtp:33 %s\r\nb=AS:5000\r\na=%s\r\n",
				 stream_id + 1, tp, running ? "sendonly" : "inactive");
		// NEEDS CHANGED
		if (!stream_id)
		{
			http_response(streamFd, 404, NULL, NULL, cseq, 0);
			goto done;
		}
		snprintf(buf, sizeof(buf), "Content-type: application/sdp\r\nContent-Base: %s",
				 request.substr(9, pos - 9).c_str());

		http_response(streamFd, 200, buf, sbuf, cseq, 0);

		goto done;
	}
	char transport_reply[300];
	transport_reply[0] = 0;

	if ((request.substr(0, 6) == "SETUP ") || (request.substr(0, 5) == "PLAY "))
	{
		char *transport = NULL, *tcp = NULL, *port = NULL;

		transport = ::strcasestr(buf, (char *)"transport:");
		tcp = ::strcasestr(buf, (char *)"RTP/AVP/TCP");
		port = ::strcasestr(buf, (char *)"client_port=");
		if (port)
			port += 12;

		if (transport && tcp)
		{
			proto = PROTO_RTSP_TCP;
			snprintf(transport_reply, sizeof(transport_reply),
					 "Transport: RTP/AVP/TCP;interleaved=0-1\r\nSession: %010d;timeout=%d\r\ncom.ses.streamID: %d",
					 session_id, 30, stream_id);
		}
		if (transport && port)
		{
			eDebug("UDP transport not supported to %s, use RTSP over TCP", (char *)m_remotehost.c_str());
			http_response(streamFd, 405, (char *)"UDP: Not supported, use rtsp over tcp", NULL, cseq, 0);
			goto done;
			/*
			   int client_port = map_intd(port, NULL, -1);
			   char *rhost = (char *)m_remotehost.c_str();
			   proto = PROTO_RTSP_UDP;
			   eDebug("attempting to connect to %s:%d",rhost, client_port);
			   remote_socket = udp_connect(rhost, client_port);
			   if(remote_socket < 0 )
			   {
			   eDebug("Could not create udp socket to %s:%d", rhost, client_port);
			   http_response(streamFd, 404, NULL, NULL, cseq, 0);
			   goto done;

			   }
			   snprintf(transport_reply, sizeof(transport_reply),
			         "Transport: RTP/AVP;unicast;destination=%s;source=%s;client_port=%d-%d;server_port=%d-%d\r\nSession: %010d;timeout=%d\r\ncom.ses.streamID: %d",
			         rhost, get_sock_shost(remote_socket),
			         client_port,
			         client_port + 1,
			   //							opts.start_rtp, opts.start_rtp + 1,
			         get_sock_sport(remote_socket),
			         get_sock_sport(remote_socket) + 1, session_id,
			         30, stream_id);
			 */
		}
		if ((request.substr(0, 6) == "SETUP "))
		{
			if (transport[0])
				http_response(streamFd, 200, transport_reply, NULL, cseq, 0);
			else
				http_response(streamFd, 454, NULL, NULL, cseq, 0);
			goto done;
		}
	}

	if (request.substr(0, 5) == "PLAY ")
	{
		if (!tune_completed && (m_serviceref.size() > 1) && (proto > 0))
		{
			eDebug("starting the stream server with string %s", m_serviceref.c_str());
			if (eDVBServiceStream::start(m_serviceref.c_str(), streamFd) >= 0)
			{
				tune_completed = true;
				m_useencoder = false;
			}
		}
		else if (!tune_completed)
		{
			eDebug("No service ref detected: <%s>, proto %d", m_serviceref.c_str(), proto);
			http_response(streamFd, 404, (char *)"Transponder: Not Found in Enigma DB, do network scan", NULL, cseq, 0);
			goto done;
		}
		else if (proto == 0)
		{
			eDebug("No (TCP) transport detected  detected: <%s>", m_serviceref.c_str());
			http_response(streamFd, 405, (char *)"rtsp_over_tcp: not specified", NULL, cseq, 0);
			goto done;
		}
		http_response(streamFd, 200, transport_reply[0] ? transport_reply : NULL, NULL, cseq, 0);

		goto done;
	}

	if (request.substr(0, 5) == "GET /")
	{
		char buf[2000];
		char reply_all[3000];
		char adapters[200];
		int len = 0;
		int tuner_s2, tuner_t, tuner_c, tuner_t2, tuner_c2;
		const char *reply = "HTTP/1.0 200 OK\r\nCACHE-CONTROL: no-cache\r\nContent-type: text/xml\r\nX-SATIP-RTSP-Port: %d\r\nContent-Length: %d\r\n\r\n%s";

		memset(buf, 0, sizeof(buf));
		getFontends(tuner_t, tuner_t2, tuner_s2, tuner_c, tuner_c2);
		memset(adapters, 0, sizeof(adapters));
		if (tuner_s2)
			len += sprintf(adapters, "DVBS2-%d,", tuner_s2);
		if (tuner_t)
			len += sprintf(adapters + len, "DVBT-%d,", tuner_t);
		if (tuner_c)
			len += sprintf(adapters + len, "DVBC-%d,", tuner_c);
		if (tuner_t2)
			len += sprintf(adapters + len, "DVBT2-%d,", tuner_t2);
		if (tuner_c2)
			len += sprintf(adapters + len, "DVBC2-%d,", tuner_c2);
		if (tuner_s2 + tuner_t + tuner_c + tuner_t2 + tuner_c2 == 0)
			len = sprintf(adapters, "DVBS2-0,");
		adapters[len - 1] = 0;
		sprintf(buf, satip_xml, m_remotehost.c_str(), tcp_port, adapters);

		len = sprintf(reply_all, reply, tcp_port, strlen(buf), buf);
		writeAll(streamFd, reply_all, len);
		rsn->stop();
		parent->connectionLost(this);
		return;
	}
	if (!running)
	{
		const char *reply = "HTTP/1.0 400 Bad Request\r\n\r\n";
		writeAll(streamFd, reply, strlen(reply));
		rsn->stop();
		parent->connectionLost(this);
		return;
	}
done:
	request.clear();
}

void eRTSPStreamClient::stopStream()
{
	ePtr<eRTSPStreamClient> ref = this;
	rsn->stop();
	parent->connectionLost(this);
}

std::string eRTSPStreamClient::getRemoteHost()
{
	return m_remotehost;
}

std::string eRTSPStreamClient::getServiceref()
{
	return m_serviceref;
}

bool eRTSPStreamClient::isUsingEncoder()
{
	return m_useencoder;
}

DEFINE_REF(eRTSPStreamServer);

eRTSPStreamServer *eRTSPStreamServer::m_instance = NULL;

eRTSPStreamServer::eRTSPStreamServer()
	: eServerSocket(tcp_port, eApp)
{
	m_instance = this;
	//	e2avahi_announce(NULL, "_e2stream._tcp", 8001);
}

eRTSPStreamServer::~eRTSPStreamServer()
{
	for (eSmartPtrList<eRTSPStreamClient>::iterator it = clients.begin(); it != clients.end();)
	{
		it = clients.erase(it);
	}
}

eRTSPStreamServer *eRTSPStreamServer::getInstance()
{
	return m_instance;
}

void eRTSPStreamServer::newConnection(int socket)
{
	ePtr<eRTSPStreamClient> client = new eRTSPStreamClient(this, socket, RemoteHost());
	clients.push_back(client);
	client->start();
}

void eRTSPStreamServer::connectionLost(eRTSPStreamClient *client)
{
	eSmartPtrList<eRTSPStreamClient>::iterator it = std::find(clients.begin(), clients.end(), client);
	if (it != clients.end())
	{
		clients.erase(it);
	}
}

void eRTSPStreamServer::stopStream()
{
	eSmartPtrList<eRTSPStreamClient>::iterator it = clients.begin();
	if (it != clients.end())
	{
		it->stopStream();
	}
}

PyObject *eRTSPStreamServer::getConnectedClients()
{
	ePyObject ret;
	int idx = 0;
	int cnt = clients.size();
	ret = PyList_New(cnt);
	for (eSmartPtrList<eRTSPStreamClient>::iterator it = clients.begin(); it != clients.end(); ++it)
	{
		ePyObject tuple = PyTuple_New(3);
		PyTuple_SET_ITEM(tuple, 0, PyString_FromString((char *)it->getRemoteHost().c_str()));
		PyTuple_SET_ITEM(tuple, 1, PyString_FromString((char *)it->getServiceref().c_str()));
		PyTuple_SET_ITEM(tuple, 2, PyInt_FromLong(it->isUsingEncoder()));
		PyList_SET_ITEM(ret, idx++, tuple);
	}
	return ret;
}

eAutoInitPtr<eRTSPStreamServer> init_eRTSPStreamServer(eAutoInitNumbers::service + 1, "RTSP Stream server");
