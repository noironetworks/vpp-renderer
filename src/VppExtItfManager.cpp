/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Copyright (c) 2018 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#include <boost/optional.hpp>

#include <opflexagent/PolicyManager.h>

#include <modelgbp/gbp/ExternalInterface.hpp>
#include <modelgbp/gbp/L3ExternalDomain.hpp>

#include <vom/gbp_subnet.hpp>
#include <vom/gbp_ext_itf.hpp>

#include "VppLog.hpp"
#include "VppUtil.hpp"
#include "VppExtItfManager.hpp"
#include "VppEndPointGroupManager.hpp"

using namespace VOM;

namespace VPP
{
ExtItfManager::ExtItfManager(Runtime &runtime)
    : m_runtime(runtime)
{
}

void
ExtItfManager::handle_update(const opflex::modb::URI &uri)
{
    OM::mark_n_sweep ms(uri.toString());
    const std::string &uuid = uri.toString();

    boost::optional<std::shared_ptr<modelgbp::gbp::ExternalInterface> > ext_itf =
        modelgbp::gbp::ExternalInterface::resolve(m_runtime.agent.getFramework(), uri);

    if (!ext_itf)
    {
        VLOGD << "External-Interface; delete: " << uri;
        return;
    }
    VLOGD << "External-Interface; update: " << uri;

    boost::optional<std::shared_ptr<modelgbp::gbp::ExternalL3BridgeDomain>> op_bd = 
      m_runtime.policy_manager().getBDForExternalInterface(uri);

    if (!op_bd)
    {
        VLOGE << "External-Interface; no ExternalBridgeDomain: " << uri;
        return;
    }

    boost::optional<std::shared_ptr<modelgbp::gbp::RoutingDomain>> op_rd =
      m_runtime.policy_manager().getRDForExternalInterface(uri);

    if (!op_rd)
    {
        VLOGE << "External-Interface; no RouteDomain: " << uri;
        return;
    }

    uint32_t rd_id =
      m_runtime.id_gen.get(modelgbp::gbp::RoutingDomain::CLASS_ID,
                                          op_rd.get()->getURI());

    route_domain rd(rd_id);
    OM::write(uuid, rd);

    uint32_t bd_id = m_runtime.id_gen.get(modelgbp::gbp::BridgeDomain::CLASS_ID,
                                          op_bd.get()->getURI());

    bridge_domain bd(bd_id, bridge_domain::learning_mode_t::OFF);
    OM::write(uuid, bd);

    /*
     * Create a BVI interface for the EPG and add it to the bridge-domain
     */
    std::shared_ptr<interface> bvi =
      EndPointGroupManager::mk_bvi(m_runtime, uuid, bd, rd,
                                   mac_from_modb(ext_itf.get()->getMac()));

    /*
     * Add the mcast tunnels for flooding
     */
    boost::optional<std::string> maddr =
      m_runtime.policy_manager().getBDMulticastIPForExternalInterface(uri);
    boost::optional<uint32_t> bd_vnid =
      m_runtime.policy_manager().getBDVnidForExternalInterface(uri);

    if (!(bd_vnid && maddr))
    {
      VLOGE << "External-Interface; no VNI/mcast-address: " << uri;
      return;
    }

    std::shared_ptr<vxlan_tunnel> vt_mc =
      EndPointGroupManager::mk_mcast_tunnel(m_runtime, uuid,
                                            bd_vnid.get(), maddr.get());

    /*
     * there's no leanring of EPs in an external BD
     */
    gbp_bridge_domain gbd(bd, *bvi, {}, vt_mc,
                          gbp_bridge_domain::flags_t::DO_NOT_LEARN);
    OM::write(uuid, gbd);
    gbp_route_domain grd(rd);
    OM::write(uuid, grd);

    /*
     * the encap on the external-interface is a vlan ID
     */
    boost::optional<uint32_t> vlan_id = ext_itf.get()->getEncap();

    if (vlan_id)
      ;

    /*
     * This BVI is the ExternalInterface
     */
    gbp_ext_itf gei(*bvi, gbd, grd);
    OM::write(uuid, gei);

    /*
     * Add any external networks
     */
    boost::optional<std::shared_ptr<modelgbp::gbp::L3ExternalDomain>> ext_dom =
        m_runtime.policy_manager().getExternalDomainForExternalInterface(uri);

    if (!ext_dom)
    {
        VLOGI << "External-Interface; no ExternalDomain: " << uri;
        return;
    }

    /* To get all the external networks in an external domain */
    std::vector<std::shared_ptr<modelgbp::gbp::L3ExternalNetwork>> ext_nets;
    ext_dom.get()->resolveGbpL3ExternalNetwork(ext_nets);

    for (std::shared_ptr<modelgbp::gbp::L3ExternalNetwork> net : ext_nets)
    {
        /* For each external network, get the sclass */
        boost::optional<uint32_t> sclass =
            m_runtime.policy_manager().getSclassForExternalNet(uri);

        if (!sclass)
          continue;

        /* Construct a fake EPG so we re-use the same model of epg-ID<->sclass
         * conversions */
        uint32_t epg_id = m_runtime.id_gen.get_ext_net_vnid(uri);
        gbp_endpoint_group gepg(epg_id, sclass.get(), grd, gbd);
        OM::write(uuid, gepg);

        /* traverse each subnet in the network */
        std::vector<std::shared_ptr<modelgbp::gbp::ExternalSubnet> > ext_subs;
        net->resolveGbpExternalSubnet(ext_subs);

        for (std::shared_ptr<modelgbp::gbp::ExternalSubnet> snet : ext_subs)
        {
            if (!snet->isAddressSet() || !snet->isPrefixLenSet())
                    continue;

            VLOGD << "External-Interface; subnet:" << uri
                  << " external:" << ext_dom.get()->getName("n/a")
                  << " external-net:" << net->getName("n/a")
                  << " external-sub:" << snet->getAddress("n/a") << "/"
                  << std::to_string(snet->getPrefixLen(99))
                  << " sclass:" << sclass.get();

            boost::asio::ip::address addr =
                boost::asio::ip::address::from_string(snet->getAddress().get());

            gbp_subnet gs(rd, {addr, snet->getPrefixLen().get()}, gepg);
            OM::write(uuid, gs);
        }
    }
}

}; // namepsace VPP

/*
 * Local Variables:
 * eval: (c-set-style "llvm.org")
 * End:
 */
