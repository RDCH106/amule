//
// This file is part of aMule Project
//
// Copyright (c) 2004-2008 Angel Vidal ( kry@amule.org )
// Copyright (c) 2004-2008 aMule Project ( http://www.amule-project.net )
// Copyright (C)2003 Barry Dunne (http://www.emule-project.net)

// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301, USA

// This work is based on the java implementation of the Kademlia protocol.
// Kademlia: Peer-to-peer routing based on the XOR metric
// Copyright (C) 2002  Petar Maymounkov [petar@post.harvard.edu]
// http://kademlia.scs.cs.nyu.edu

// Note To Mods //
/*
Please do not change anything here and release it..
There is going to be a new forum created just for the Kademlia side of the client..
If you feel there is an error or a way to improve something, please
post it in the forum first and let us look at it.. If it is a real improvement,
it will be added to the offical client.. Changing something without knowing
what all it does can cause great harm to the network if released in mass form..
Any mod that changes anything within the Kademlia side will not be allowed to advertise
there client on the eMule forum..
*/

#include "RoutingBin.h"
#include "../../Logger.h"
#include "../../NetworkFunctions.h"
#include "../../RandomFunctions.h"

////////////////////////////////////////
using namespace Kademlia;
////////////////////////////////////////

CRoutingBin::GlobalTrackingMap	CRoutingBin::s_globalContactIPs;
CRoutingBin::GlobalTrackingMap	CRoutingBin::s_globalContactSubnets;

CRoutingBin::~CRoutingBin()
{
	for (ContactList::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		AdjustGlobalTracking((*it)->GetIPAddress(), false);
		if (!m_dontDeleteContacts) {
			delete *it;
		}
	}

	m_entries.clear();
}

bool CRoutingBin::AddContact(CContact *contact)
{
	wxASSERT(contact != NULL);

	uint32_t sameSubnets = 0;
	// Check if we already have a contact with this ID in the list.
	for (ContactList::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		if (contact->GetClientID() == (*it)->GetClientID()) {
			return false;
		}
		if ((contact->GetIPAddress() & 0xFFFFFF00) == ((*it)->GetIPAddress() & 0xFFFFFF00)) {
			sameSubnets++;
		}
	}
	// Several checks to make sure that we don't store multiple contacts from the same IP or too many contacts from the same subnet
	// This is supposed to add a bit of protection against several attacks and raise the resource needs (IPs) for a successful contact on the attacker side
	// Such IPs are not banned from Kad, they still can index, search, etc so multiple KAD clients behind one IP still work

	// no more than 1 KadID per IP
	uint32_t sameIPCount = 0;
	GlobalTrackingMap::const_iterator itIP = s_globalContactIPs.find(contact->GetIPAddress());
	if (itIP != s_globalContactIPs.end()) {
		sameIPCount = itIP->second;
	}
	if (sameIPCount >= 1) {
		AddDebugLogLineM(false, logKadRouting, wxT("Ignored kad contact (IP=") + Uint32_16toStringIP_Port(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress()), contact->GetUDPPort()) + wxT(") - too many contacts with the same IP (global)"));
		return false;
	}

	// no more than 2 IPs from the same /24 netmask in one bin, except if its a LANIP (if we don't accept LANIPs they already have been filtered before)
	if (sameSubnets >= 2 && !::IsLanIP(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress()))) {
		AddDebugLogLineM(false, logKadRouting, wxT("Ignored kad contact (IP=") + Uint32_16toStringIP_Port(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress()), contact->GetUDPPort()) + wxT(") - too many contact with the same subnet in RoutingBin"));
		return false;
	}

	//  no more than 10 IPs from the same /24 netmask global, except if its a LANIP (if we don't accept LANIPs they already have been filtered before)
	uint32_t sameSubnetGlobalCount = 0;
	GlobalTrackingMap::const_iterator itSubnet = s_globalContactSubnets.find(contact->GetIPAddress() & 0xFFFFFF00);
	if (itSubnet != s_globalContactSubnets.end()) {
		sameSubnetGlobalCount = itSubnet->second;
	}
	if (sameSubnetGlobalCount >= 10 && !::IsLanIP(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress()))) {
		AddDebugLogLineM(false, logKadRouting, wxT("Ignored kad contact (IP=") + Uint32_16toStringIP_Port(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress()), contact->GetUDPPort()) + wxT(") - too many contacts with the same subnet (global)"));
		return false;
	}

	// If not full, add to the end of list
	if (m_entries.size() < K) {
		m_entries.push_back(contact);
		AdjustGlobalTracking(contact->GetIPAddress(), true);
		return true;
	}
	return false;
}

void CRoutingBin::SetAlive(CContact *contact)
{
	wxASSERT(contact != NULL);
	// Check if we already have a contact with this ID in the list.
	CContact *test = GetContact(contact->GetClientID());
	wxASSERT(contact == test);
	if (test) {
		// Mark contact as being alive.
		test->UpdateType();
		// Move to the end of the list
		PushToBottom(test);
	}
}

void CRoutingBin::SetTCPPort(uint32_t ip, uint16_t port, uint16_t tcpPort)
{
	// Find contact with IP/Port
	for (ContactList::iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		CContact *c = *it;
		if ((ip == c->GetIPAddress()) && (port == c->GetUDPPort())) {
			// Set TCPPort and mark as alive.
			c->SetTCPPort(tcpPort);
			c->UpdateType();
			// Move to the end of the list
			PushToBottom(c);
			break;
		}
	}
}

CContact *CRoutingBin::GetContact(const CUInt128 &id) const throw()
{
	for (ContactList::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		if ((*it)->GetClientID() == id) {
			return *it;
		}
	}
	return NULL;
}

CContact *CRoutingBin::GetContact(uint32_t ip, uint16_t port, bool tcpPort) const throw()
{
	for (ContactList::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		CContact *contact = *it;
		if ((contact->GetIPAddress() == ip)
		    && ((!tcpPort && port == contact->GetUDPPort()) || (tcpPort && port == contact->GetTCPPort()) || port == 0)) {
			return contact;
		}
	}
	return NULL;
}

void CRoutingBin::GetEntries(ContactList *result, bool emptyFirst) const
{
	// Clear results if requested first.
	if (emptyFirst) {
		result->clear();
	}

	// Append all entries to the results.
	if (m_entries.size() > 0) {
		result->insert(result->end(), m_entries.begin(), m_entries.end());
	}
}

void CRoutingBin::GetClosestTo(uint32_t maxType, const CUInt128 &target, uint32_t maxRequired, ContactMap *result, bool emptyFirst, bool inUse) const
{
	// Empty list if requested.
	if (emptyFirst) {
		result->clear();
	}

	// No entries, no closest.
	if (m_entries.size() == 0) {
		return;
	}

	// First put results in sort order for target so we can insert them correctly.
	// We don't care about max results at this time.
	for (ContactList::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		if ((*it)->GetType() <= maxType) {
			CUInt128 targetDistance((*it)->GetClientID() ^ target);
			(*result)[targetDistance] = *it;
			// This list will be used for an unknown time, Inc in use so it's not deleted.
			if (inUse) {
				(*it)->IncUse();
			}
		}
	}

	// Remove any extra results by least wanted first.
	while (result->size() > maxRequired) {
		// Dec in use count.
 		if (inUse) {
  			(--result->end())->second->DecUse();
		}
 		// Remove from results
 		result->erase(--result->end());
	}
}

void CRoutingBin::AdjustGlobalTracking(uint32_t ip, bool increase)
{
	// IP
	uint32_t sameIPCount = 0;
	GlobalTrackingMap::const_iterator itIP = s_globalContactIPs.find(ip);
	if (itIP != s_globalContactIPs.end()) {
		sameIPCount = itIP->second;
	}
	if (increase) {
		if (sameIPCount >= 1) {
			AddDebugLogLineM(false, logKadRouting, wxT("Global IP Tracking inconsistency on increase (") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(ip)) + wxT(")"));
			wxFAIL;
		}
		sameIPCount++;
	} else /* if (!increase) */ {
		if (sameIPCount == 0) {
			AddDebugLogLineM(false, logKadRouting, wxT("Global IP Tracking inconsistency on decrease (") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(ip)) + wxT(")"));
			wxFAIL;
		}
		sameIPCount--;
	}
	if (sameIPCount != 0) {
		s_globalContactIPs[ip] = sameIPCount;
	} else {
		s_globalContactIPs.erase(ip);
	}

	// Subnet
	uint32_t sameSubnetCount = 0;
	GlobalTrackingMap::const_iterator itSubnet = s_globalContactSubnets.find(ip & 0xFFFFFF00);
	if (itSubnet != s_globalContactSubnets.end()) {
		sameSubnetCount = itSubnet->second;
	}
	if (increase) {
		if (sameSubnetCount >= 10 && !::IsLanIP(wxUINT32_SWAP_ALWAYS(ip))) {
			AddDebugLogLineM(false, logKadRouting, wxT("Global Subnet Tracking inconsistency on increase (") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(ip)) + wxT("/24)"));
			wxFAIL;
		}
		sameSubnetCount++;
	} else /* if (!increase) */ {
		if (sameSubnetCount == 0) {
			AddDebugLogLineM(false, logKadRouting, wxT("Global Subnet Tracking inconsistency on decrease (") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(ip)) + wxT("/24)"));
			wxFAIL;
		}
		sameSubnetCount--;
	}
	if (sameSubnetCount != 0) {
		s_globalContactSubnets[ip & 0xFFFFFF00] = sameSubnetCount;
	} else {
		s_globalContactSubnets.erase(ip & 0xFFFFFF00);
	}
}

bool CRoutingBin::ChangeContactIPAddress(CContact *contact, uint32_t newIP)
{
	// Called if we want to update an indexed contact with a new IP. We have to check if we actually allow such a change
	// and if adjust our tracking. Rejecting a change will in the worst case lead a node contact to become invalid and purged later,
	// but it also protects against a flood of malicous update requests from one IP which would be able to "reroute" all
	// contacts to itself and by that making them useless
	if (contact->GetIPAddress() == newIP) {
		return true;
	}

	wxASSERT(GetContact(contact->GetClientID()) == contact);

	// no more than 1 KadID per IP
	uint32_t sameIPCount = 0;
	GlobalTrackingMap::const_iterator itIP = s_globalContactIPs.find(newIP);
	if (itIP != s_globalContactIPs.end()) {
		sameIPCount = itIP->second;
	}
	if (sameIPCount >= 1) {
		AddDebugLogLineM(false, logKadRouting, wxT("Rejected kad contact update on IP change (old IP=") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress())) + wxT(", requested IP=") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(newIP)) + wxT(") - too many contacts with the same IP (global)"));
		return false;
	}

	if ((contact->GetIPAddress() & 0xFFFFFF00) != (newIP & 0xFFFFFF00)) {
		// no more than 10 IPs from the same /24 netmask global, except if it's a LAN IP (if we don't accept LAN IPs they already have been filtered before)
		uint32_t sameSubnetGlobalCount = 0;
		GlobalTrackingMap::const_iterator itGlobalSubnet = s_globalContactSubnets.find(newIP & 0xFFFFFF00);
		if (itGlobalSubnet != s_globalContactSubnets.end()) {
			sameSubnetGlobalCount = itGlobalSubnet->second;
		}
		if (sameSubnetGlobalCount >= 10 && !::IsLanIP(wxUINT32_SWAP_ALWAYS(newIP))) {
			AddDebugLogLineM(false, logKadRouting, wxT("Rejected kad contact update on IP change (old IP=") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress())) + wxT(", requested IP=") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(newIP)) + wxT(") - too many contacts with the same Subnet (global)"));
			return false;
		}

		// no more than 2 IPs from the same /24 netmask in one bin, except if it's a LAN IP (if we don't accept LAN IPs they already have been filtered before)
		uint32_t sameSubnets = 0;
		// Check if we already have a contact with this ID in the list.
		for (ContactList::const_iterator itContact = m_entries.begin(); itContact != m_entries.end(); ++itContact) {
			if ((newIP & 0xFFFFFF00) == ((*itContact)->GetIPAddress() & 0xFFFFFF00)) {
				sameSubnets++;
			}
		}
		if (sameSubnets >= 2 && !::IsLanIP(wxUINT32_SWAP_ALWAYS(newIP))) {
			AddDebugLogLineM(false, logKadRouting, wxT("Rejected kad contact update on IP change (old IP=") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress())) + wxT(", requested IP=") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(newIP)) + wxT(") - too many contacts with the same Subnet (local)"));
			return false;
		}
	}

	// everything fine
	AddDebugLogLineM(false, logKadRouting, wxT("Index contact IP change allowed ") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(contact->GetIPAddress())) + wxT(" -> ") + Uint32toStringIP(wxUINT32_SWAP_ALWAYS(newIP)));
	AdjustGlobalTracking(contact->GetIPAddress(), false);
	contact->SetIPAddress(newIP);
	AdjustGlobalTracking(contact->GetIPAddress(), true);
	return true;
}

void CRoutingBin::PushToBottom(CContact *contact)
{
	wxASSERT(GetContact(contact->GetClientID()) == contact);

	RemoveContact(contact, true);
	m_entries.push_back(contact);
}

CContact *CRoutingBin::GetRandomContact(uint32_t maxType, uint32_t minKadVersion) const throw()
{
	if (m_entries.empty()) {
		return NULL;
	}

	// Find contact
	CContact *lastFit = NULL;
	uint32_t randomStartPos = GetRandomUint16() % m_entries.size();
	uint32_t index = 0;

	for (ContactList::const_iterator it = m_entries.begin(); it != m_entries.end(); ++it) {
		if ((*it)->GetType() <= maxType && (*it)->GetVersion() >= minKadVersion) {
			if (index >= randomStartPos) {
				return *it;
			} else {
				lastFit = *it;
			}
		}
		index++;
	}

	return lastFit;
}
