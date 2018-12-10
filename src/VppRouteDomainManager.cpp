/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Copyright (c) 2018 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#include <boost/optional.hpp>

#include <modelgbp/gbp/L3ExternalDomain.hpp>
#include <modelgbp/gbp/L3ExternalNetwork.hpp>
#include <modelgbp/gbp/RoutingDomain.hpp>

#include <opflexagent/RDConfig.h>

#include <vom/bridge_domain.hpp>
#include <vom/gbp_contract.hpp>
#include <vom/gbp_endpoint.hpp>
#include <vom/gbp_endpoint_group.hpp>
#include <vom/gbp_recirc.hpp>
#include <vom/gbp_subnet.hpp>
#include <vom/interface.hpp>
#include <vom/l2_binding.hpp>
#include <vom/l3_binding.hpp>
#include <vom/nat_binding.hpp>
#include <vom/om.hpp>
#include <vom/om.hpp>
#include <vom/route.hpp>
#include <vom/route_domain.hpp>
#include <vom/sub_interface.hpp>

#include "VppEndPointGroupManager.hpp"
#include "VppLog.hpp"
#include "VppRouteDomainManager.hpp"

using namespace VOM;

namespace VPP
{
RouteDomainManager::RouteDomainManager(opflexagent::Agent &agent,
                                       IdGen &id_gen,
                                       Uplink &uplink)
    : m_agent(agent)
    , m_id_gen(id_gen)
    , m_uplink(uplink)
{
}

opflexagent::network::subnets_t
get_rd_subnets(opflexagent::Agent &agent, const opflex::modb::URI &uri)
{
    /*
     * this is a cut-n-paste from IntflowManager.
     */
    opflexagent::network::subnets_t intSubnets;

    boost::optional<std::shared_ptr<modelgbp::gbp::RoutingDomain>> rd =
        modelgbp::gbp::RoutingDomain::resolve(agent.getFramework(), uri);

    if (!rd)
    {
        return intSubnets;
    }

    std::vector<std::shared_ptr<modelgbp::gbp::RoutingDomainToIntSubnetsRSrc>>
        subnets_list;
    rd.get()->resolveGbpRoutingDomainToIntSubnetsRSrc(subnets_list);
    for (auto &subnets_ref : subnets_list)
    {
        boost::optional<opflex::modb::URI> subnets_uri =
            subnets_ref->getTargetURI();
        opflexagent::PolicyManager::resolveSubnets(
            agent.getFramework(), subnets_uri, intSubnets);
    }
    std::shared_ptr<const opflexagent::RDConfig> rdConfig =
        agent.getExtraConfigManager().getRDConfig(uri);
    if (rdConfig)
    {
        for (const std::string &cidrSn : rdConfig->getInternalSubnets())
        {
            opflexagent::network::cidr_t cidr;
            if (opflexagent::network::cidr_from_string(cidrSn, cidr))
            {
                intSubnets.insert(
                    make_pair(cidr.first.to_string(), cidr.second));
            }
            else
            {
                VLOGE << "Invalid CIDR subnet: " << cidrSn;
            }
        }
    }

    return intSubnets;
}

void
RouteDomainManager::handle_update(const opflex::modb::URI &uri)
{
    OM::mark_n_sweep ms(uri.toString());

    boost::optional<std::shared_ptr<modelgbp::gbp::RoutingDomain>> op_opf_rd =
        modelgbp::gbp::RoutingDomain::resolve(m_agent.getFramework(), uri);

    if (!op_opf_rd)
    {
        VLOGD << "Cleaning up for RD: " << uri;
        m_id_gen.erase(modelgbp::gbp::RoutingDomain::CLASS_ID, uri);
        return;
    }
    std::shared_ptr<modelgbp::gbp::RoutingDomain> opf_rd = op_opf_rd.get();

    const std::string &rd_uuid = uri.toString();

    VLOGD << "Importing routing domain:" << uri;

    /*
     * get all the subnets that are internal to this route domain
     */
    opflexagent::network::subnets_t intSubnets = get_rd_subnets(m_agent, uri);
    boost::system::error_code ec;

    /*
     * create (or at least own) VPP's route-domain object
     */
    uint32_t rdId = m_id_gen.get(modelgbp::gbp::RoutingDomain::CLASS_ID, uri);

    VOM::route_domain rd(rdId);
    VOM::OM::write(rd_uuid, rd);

    /*
     * For each internal Subnet
     */
    for (const auto &sn : intSubnets)
    {
        /*
         * still a little more song and dance before we can get
         * our hands on an address ...
         */
        boost::asio::ip::address addr =
            boost::asio::ip::address::from_string(sn.first, ec);
        if (ec) continue;

        VLOGD << "Importing routing domain:" << uri << " subnet:" << addr << "/"
              << std::to_string(sn.second);

        /*
         * add a route for the subnet in VPP's route-domain via
         * the EPG's uplink, DVR styleee
         */
        gbp_subnet gs(
            rd, {addr, sn.second}, gbp_subnet::type_t::STITCHED_INTERNAL);
        OM::write(rd_uuid, gs);
    }

    /*
     * for each external subnet
     */
    std::vector<std::shared_ptr<modelgbp::gbp::L3ExternalDomain>> extDoms;
    opf_rd.get()->resolveGbpL3ExternalDomain(extDoms);
    for (std::shared_ptr<modelgbp::gbp::L3ExternalDomain> &extDom : extDoms)
    {
        std::vector<std::shared_ptr<modelgbp::gbp::L3ExternalNetwork>> extNets;
        extDom->resolveGbpL3ExternalNetwork(extNets);

        for (std::shared_ptr<modelgbp::gbp::L3ExternalNetwork> net : extNets)
        {
            std::vector<std::shared_ptr<modelgbp::gbp::ExternalSubnet>> extSubs;
            net->resolveGbpExternalSubnet(extSubs);
            boost::optional<std::shared_ptr<
                modelgbp::gbp::L3ExternalNetworkToNatEPGroupRSrc>>
                natRef = net->resolveGbpL3ExternalNetworkToNatEPGroupRSrc();
            boost::optional<uint32_t> natEpgVnid = boost::none;
            boost::optional<opflex::modb::URI> natEpg = boost::none;

            if (natRef)
            {
                natEpg = natRef.get()->getTargetURI();
                if (natEpg)
                    natEpgVnid = m_agent.getPolicyManager().getVnidForGroup(
                        natEpg.get());
            }

            for (auto extSub : extSubs)
            {
                if (!extSub->isAddressSet() || !extSub->isPrefixLenSet())
                    continue;

                VLOGD << "Importing routing domain:" << uri
                      << " external:" << extDom->getName("n/a")
                      << " external-net:" << net->getName("n/a")
                      << " external-sub:" << extSub->getAddress("n/a") << "/"
                      << std::to_string(extSub->getPrefixLen(99))
                      << " nat-epg:" << natEpg << " nat-epg-id:" << natEpgVnid;

                boost::asio::ip::address addr =
                    boost::asio::ip::address::from_string(
                        extSub->getAddress().get(), ec);
                if (ec) continue;

                if (natEpgVnid)
                {
                    /*
                     * there's a NAT EPG for this subnet. create its RD, BD
                     * and EPG.
                     */
                    try
                    {
                        EndPointGroupManager::ForwardInfo nat_fwd;

                        nat_fwd = EndPointGroupManager::get_fwd_info(
                            m_agent, m_id_gen, natEpg.get());

                        VOM::route_domain nat_rd(nat_fwd.rdId);
                        VOM::OM::write(rd_uuid, nat_rd);
                        VOM::bridge_domain nat_bd(nat_fwd.bdId);
                        VOM::OM::write(rd_uuid, nat_bd);

                        std::shared_ptr<VOM::interface> encap_link =
                            m_uplink.mk_interface(rd_uuid, natEpgVnid.get());

                        gbp_endpoint_group nat_epg(
                            natEpgVnid.get(), *encap_link, nat_rd, nat_bd);
                        OM::write(rd_uuid, nat_epg);

                        /*
                         * The external-subnet is a route via the NAT-EPG's
                         recirc.
                         * the recirc is a NAT outside interface to get NAT
                         applied
                         * in-2out
                         */

                        /* setup the recirc interface */
                        VOM::interface nat_recirc_itf(
                            "recirc-" + std::to_string(natEpgVnid.get()),
                            interface::type_t::LOOPBACK,
                            VOM::interface::admin_state_t::UP,
                            nat_rd);
                        OM::write(rd_uuid, nat_recirc_itf);

                        l2_binding nat_recirc_l2b(nat_recirc_itf, nat_bd);
                        OM::write(rd_uuid, nat_recirc_l2b);

                        nat_binding nat_recirc_nb4(
                            nat_recirc_itf,
                            direction_t::INPUT,
                            l3_proto_t::IPV4,
                            nat_binding::zone_t::OUTSIDE);
                        OM::write(rd_uuid, nat_recirc_nb4);

                        nat_binding nat_recirc_nb6(
                            nat_recirc_itf,
                            direction_t::INPUT,
                            l3_proto_t::IPV6,
                            nat_binding::zone_t::OUTSIDE);
                        OM::write(rd_uuid, nat_recirc_nb6);

                        gbp_recirc nat_grecirc(nat_recirc_itf,
                                               gbp_recirc::type_t::EXTERNAL,
                                               nat_epg);
                        OM::write(rd_uuid, nat_grecirc);

                        /* add the route for the ext-subnet */
                        gbp_subnet gs(rd,
                                      {addr, extSub->getPrefixLen().get()},
                                      nat_grecirc,
                                      nat_epg);
                        OM::write(rd_uuid, gs);
                    }
                    catch (EndPointGroupManager::NoFowardInfoException &nofwd)
                    {
                        VLOGD << "EndpointGroup - no fwding " << uri;
                    }
                }
                else
                {
                    /*
                     * through this EPG's uplink port
                     */
                    gbp_subnet gs(rd,
                                  {addr, extSub->getPrefixLen().get()},
                                  gbp_subnet::type_t::STITCHED_INTERNAL);
                    OM::write(rd_uuid, gs);
                }
            }
        }
    }
}

}; // namepsace VPP

/*
 * Local Variables:
 * eval: (c-set-style "llvm.org")
 * End:
 */