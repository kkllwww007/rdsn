#include <dsn/utility/extensible_object.h>
#include "server_load_balancer.h"

#ifdef __TITLE__
#undef __TITLE__
#endif
#define __TITLE__ "server.load.balancer"

namespace dsn {
namespace replication {

/// server load balancer extensions for node_state
/// record the newly assigned but not finished replicas for each node, to make the assigning
/// process more balanced.
class newly_partitions
{
public:
    newly_partitions();
    newly_partitions(node_state *ns);
    node_state *owner;
    std::map<int32_t, int32_t> primaries;
    std::map<int32_t, int32_t> partitions;

    int32_t primary_count(int32_t app_id)
    {
        return owner->primary_count(app_id) + primaries[app_id];
    }
    int32_t partition_count(int32_t app_id)
    {
        return owner->partition_count(app_id) + partitions[app_id];
    }

    bool less_primaries(newly_partitions &another, int32_t app_id);
    bool less_partitions(newly_partitions &another, int32_t app_id);
    void newly_add_primary(int32_t app_id);
    void newly_add_partition(int32_t app_id);

    void newly_remove_primary(int32_t app_id);
    void newly_remove_partition(int32_t app_id);

public:
    static void *s_create(void *related_ns);
    static void s_delete(void *_this);
};
typedef dsn::object_extension_helper<newly_partitions, node_state> newly_partitions_ext;

newly_partitions::newly_partitions() : newly_partitions(nullptr) {}

newly_partitions::newly_partitions(node_state *ns) : owner(ns) {}

void *newly_partitions::s_create(void *related_ns)
{
    newly_partitions *result = new newly_partitions(reinterpret_cast<node_state *>(related_ns));
    return result;
}

void newly_partitions::s_delete(void *_this) { delete reinterpret_cast<newly_partitions *>(_this); }

bool newly_partitions::less_primaries(newly_partitions &another, int32_t app_id)
{
    int newly_p1 = primary_count(app_id);
    int newly_p2 = another.primary_count(app_id);
    if (newly_p1 != newly_p2)
        return newly_p1 < newly_p2;

    newly_p1 = partition_count(app_id);
    newly_p2 = another.partition_count(app_id);
    return newly_p1 < newly_p2;
}

bool newly_partitions::less_partitions(newly_partitions &another, int32_t app_id)
{
    int newly_p1 = partition_count(app_id);
    int newly_p2 = another.partition_count(app_id);
    return newly_p1 < newly_p2;
}

void newly_partitions::newly_add_primary(int32_t app_id)
{
    ++primaries[app_id];
    ++partitions[app_id];
}

void newly_partitions::newly_add_partition(int32_t app_id) { ++partitions[app_id]; }

void newly_partitions::newly_remove_primary(int32_t app_id)
{
    auto iter = primaries.find(app_id);
    dassert(iter != primaries.end(), "invalid app_id, app_id = %d", app_id);
    dassert(iter->second > 0, "invalid primary count, cnt = %d", iter->second);
    if (0 == (--iter->second)) {
        primaries.erase(iter);
    }

    auto iter2 = partitions.find(app_id);
    dassert(iter2 != partitions.end(), "invalid app_id, app_id = %d", app_id);
    dassert(iter2->second > 0, "invalid partition count, cnt = %d", iter2->second);
    if (0 == (--iter2->second)) {
        partitions.erase(iter2);
    }
}

void newly_partitions::newly_remove_partition(int32_t app_id)
{
    auto iter = partitions.find(app_id);
    dassert(iter != partitions.end(), "invalid app_id, app_id = %d", app_id);
    dassert(iter->second > 0, "invalid partition count, cnt = %d", iter->second);
    if ((--iter->second) == 0) {
        partitions.erase(iter);
    }
}

inline newly_partitions *get_newly_partitions(node_mapper &mapper, const dsn::rpc_address &addr)
{
    node_state *ns = get_node_state(mapper, addr, false);
    if (ns == nullptr)
        return nullptr;
    return newly_partitions_ext::get_inited(ns);
}

class local_module_initializer
{
private:
    local_module_initializer()
    {
        newly_partitions_ext::register_ext(newly_partitions::s_create, newly_partitions::s_delete);
    }

public:
    static local_module_initializer _instance;
};
local_module_initializer local_module_initializer::_instance;
//// end of server load balancer extensions for node state

server_load_balancer::server_load_balancer(meta_service *svc) : _svc(svc)
{
    _recent_choose_primary_fail_count.init(
        "eon.server_load_balancer",
        "recent_choose_primary_fail_count",
        COUNTER_TYPE_VOLATILE_NUMBER,
        "choose primary fail count in the recent period");
}

int server_load_balancer::suggest_alive_time(config_type::type t)
{
    switch (t) {
    case config_type::CT_ASSIGN_PRIMARY:
    case config_type::CT_UPGRADE_TO_PRIMARY:
    case config_type::CT_DOWNGRADE_TO_INACTIVE:
    case config_type::CT_REMOVE:
    case config_type::CT_DOWNGRADE_TO_SECONDARY:
        // this should be fast, 1 minutes is enough
        return 60;

    case config_type::CT_ADD_SECONDARY:
    case config_type::CT_ADD_SECONDARY_FOR_LB:
        return _svc->get_meta_options().add_secondary_proposal_alive_time_seconds;
    default:
        dassert(false, "invalid config_type, type = %s", ::dsn::enum_to_string(t));
        return 0;
    }
}

void server_load_balancer::register_proposals(meta_view view,
                                              const configuration_balancer_request &req,
                                              configuration_balancer_response &resp)
{
    config_context &cc = *get_config_context(*view.apps, req.gpid);
    partition_configuration &pc = *get_config(*view.apps, req.gpid);
    if (!cc.lb_actions.empty()) {
        resp.err = ERR_INVALID_PARAMETERS;
        return;
    }

    cc.lb_actions.acts = req.action_list;
    cc.lb_actions.from_balancer = true;
    for (configuration_proposal_action &act : cc.lb_actions.acts) {
        if (act.target.is_invalid())
            act.target = pc.primary;

        if (act.period_ts == 0)
            act.period_ts = suggest_alive_time(act.type);
        act.period_ts += (dsn_now_ms() / 1000);
    }
    resp.err = ERR_OK;
    return;
}

void server_load_balancer::apply_balancer(meta_view view, const migration_list &ml)
{
    if (!ml.empty()) {
        configuration_balancer_response resp;
        for (auto &pairs : ml) {
            register_proposals(view, *pairs.second, resp);
            if (resp.err != dsn::ERR_OK) {
                const dsn::gpid &pid = pairs.first;
                dassert(false,
                        "apply balancer for gpid(%d.%d) failed",
                        pid.get_app_id(),
                        pid.get_partition_index());
            }
        }
    }
}

void simple_load_balancer::reconfig(meta_view view, const configuration_update_request &request)
{
    const dsn::gpid &gpid = request.config.pid;
    if (!((*view.apps)[gpid.get_app_id()]->is_stateful)) {
        return;
    }

    config_context *cc = get_config_context(*(view.apps), gpid);
    partition_configuration *pc = get_config(*(view.apps), gpid);

    if (!cc->lb_actions.empty()) {
        if (cc->lb_actions.acts.size() == 1) {
            reset_proposal(view, gpid);
        } else {
            cc->lb_actions.pop_front();
        }
    }

    // handle the dropped out servers
    if (request.type == config_type::CT_DROP_PARTITION) {
        cc->serving.clear();

        const std::vector<rpc_address> &config_dropped = request.config.last_drops;
        for (const rpc_address &drop_node : config_dropped) {
            cc->record_drop_history(drop_node);
        }
    } else {
        when_update_replicas(request.type, [cc, pc, &request](bool is_adding) {
            if (is_adding) {
                cc->remove_from_dropped(request.node);
                // when some replicas are added to partition_config
                // we should try to adjust the size of drop_list
                cc->check_size();
            } else {
                cc->remove_from_serving(request.node);

                dassert(cc->record_drop_history(request.node),
                        "node(%s) has been in the dropped",
                        request.node.to_string());
            }
        });
    }
}

void simple_load_balancer::reset_proposal(meta_view &view, const dsn::gpid &gpid)
{
    config_context *cc = get_config_context(*(view.apps), gpid);
    dassert(!cc->lb_actions.empty(), "");

    configuration_proposal_action &act = cc->lb_actions.acts.front();
    newly_partitions *np = get_newly_partitions(*(view.nodes), act.node);
    if (np == nullptr) {
        ddebug("can't get the newly_partitions extension structure for node(%s), "
               "the node may be dead and removed",
               act.node.to_string());
    } else if (!cc->lb_actions.from_balancer) {
        when_update_replicas(act.type, [np, &gpid](bool is_adding) {
            if (is_adding)
                np->newly_remove_partition(gpid.get_app_id());
        });
    }

    cc->lb_actions.clear();
    cc->lb_actions.from_balancer = false;
}

bool simple_load_balancer::from_proposals(meta_view &view,
                                          const dsn::gpid &gpid,
                                          configuration_proposal_action &action)
{
    const partition_configuration &pc = *get_config(*(view.apps), gpid);
    config_context &cc = *get_config_context(*(view.apps), gpid);
    bool is_action_valid;

    if (cc.lb_actions.empty()) {
        action.type = config_type::CT_INVALID;
        return false;
    }
    action = cc.lb_actions.acts.front();
    char reason[1024];
    if (action.target.is_invalid()) {
        sprintf(reason, "action target is invalid");
        goto invalid_action;
    }
    if (action.node.is_invalid()) {
        sprintf(reason, "action node is invalid");
        goto invalid_action;
    }
    if (!is_node_alive(*(view.nodes), action.target)) {
        sprintf(reason, "action target(%s) is not alive", action.target.to_string());
        goto invalid_action;
    }
    if (!is_node_alive(*(view.nodes), action.node)) {
        sprintf(reason, "action node(%s) is not alive", action.node.to_string());
        goto invalid_action;
    }
    if (has_seconds_expired(action.period_ts)) {
        sprintf(reason, "action time expired");
        goto invalid_action;
    }

    switch (action.type) {
    case config_type::CT_ASSIGN_PRIMARY:
        is_action_valid = (action.node == action.target && pc.primary.is_invalid() &&
                           !is_secondary(pc, action.node));
        break;
    case config_type::CT_UPGRADE_TO_PRIMARY:
        is_action_valid = (action.node == action.target && pc.primary.is_invalid() &&
                           is_secondary(pc, action.node));
        break;
    case config_type::CT_ADD_SECONDARY:
    case config_type::CT_ADD_SECONDARY_FOR_LB:
        is_action_valid = (is_primary(pc, action.target) && !is_secondary(pc, action.node));
        is_action_valid = (is_action_valid && is_node_alive(*(view.nodes), action.node));
        break;
    case config_type::CT_DOWNGRADE_TO_INACTIVE:
    case config_type::CT_REMOVE:
        is_action_valid = (is_primary(pc, action.target) && is_member(pc, action.node));
        break;
    case config_type::CT_DOWNGRADE_TO_SECONDARY:
        is_action_valid = (action.target == action.node && is_primary(pc, action.target));
        break;
    default:
        is_action_valid = false;
        break;
    }

    if (is_action_valid)
        return true;
    else
        sprintf(reason, "action is invalid");

invalid_action:
    std::stringstream ss;
    ss << action;
    ddebug("proposal action(%s) for gpid(%d.%d) is invalid, clear all proposal actions: %s",
           ss.str().c_str(),
           gpid.get_app_id(),
           gpid.get_partition_index(),
           reason);
    action.type = config_type::CT_INVALID;

    reset_proposal(view, gpid);
    return false;
}

pc_status simple_load_balancer::on_missing_primary(meta_view &view, const dsn::gpid &gpid)
{
    const partition_configuration &pc = *get_config(*(view.apps), gpid);
    proposal_actions &acts = get_config_context(*view.apps, gpid)->lb_actions;

    char gpid_name[64];
    snprintf(gpid_name, 64, "%d.%d", gpid.get_app_id(), gpid.get_partition_index());

    configuration_proposal_action action;
    pc_status result = pc_status::invalid;

    action.type = config_type::CT_INVALID;
    // try to upgrade a secondary to primary if the primary is missing
    if (pc.secondaries.size() > 0) {
        action.node.set_invalid();

        for (int i = 0; i < pc.secondaries.size(); ++i) {
            node_state *ns = get_node_state(*(view.nodes), pc.secondaries[i], false);
            dassert(ns != nullptr,
                    "invalid secondary address, address = %s",
                    pc.secondaries[i].to_string());
            if (!ns->alive())
                continue;

            // find a node with minimal primaries
            newly_partitions *np = newly_partitions_ext::get_inited(ns);
            if (action.node.is_invalid() ||
                np->less_primaries(*get_newly_partitions(*(view.nodes), action.node),
                                   gpid.get_app_id())) {
                action.node = ns->addr();
            }
        }

        if (action.node.is_invalid()) {
            derror("all nodes for gpid(%s) are dead, waiting for some secondary to come back....",
                   gpid_name);
            result = pc_status::dead;
        } else {
            action.type = config_type::CT_UPGRADE_TO_PRIMARY;
            newly_partitions *np = get_newly_partitions(*(view.nodes), action.node);
            np->newly_add_primary(gpid.get_app_id());

            action.target = action.node;
            action.period_ts = suggest_alive_time(action.type);
            result = pc_status::ill;
        }
    }
    // if nothing in the last_drops, it means that this is a newly created partition, so let's
    // just find a node and assign primary for it.
    else if (pc.last_drops.empty()) {
        dsn::rpc_address min_primary_server;
        newly_partitions *min_primary_server_np = nullptr;

        for (auto &pairs : *view.nodes) {
            node_state &ns = pairs.second;
            if (!ns.alive())
                continue;
            newly_partitions *np = newly_partitions_ext::get_inited(&ns);
            // find a node which has minimal primaries
            if (min_primary_server_np == nullptr ||
                np->less_primaries(*min_primary_server_np, gpid.get_app_id())) {
                min_primary_server = ns.addr();
                min_primary_server_np = np;
            }
        }

        if (min_primary_server_np != nullptr) {
            action.node = min_primary_server;
            action.target = action.node;
            action.type = config_type::CT_ASSIGN_PRIMARY;
            min_primary_server_np->newly_add_primary(gpid.get_app_id());
            action.period_ts = suggest_alive_time(action.type);
        }

        result = pc_status::ill;
    }
    // well, all replicas in this partition is dead
    else {
        dwarn("%s enters DDD state, we are waiting for all replicas to come back, "
              "and select primary according to informations collected",
              gpid_name);
        // when considering how to handle the DDD state, we must keep in mind that our
        // shared/private-log data only write to OS-cache.
        // so the last removed replica can't act as primary directly.
        config_context &cc = *get_config_context(*view.apps, gpid);
        action.node.set_invalid();
        for (int i = 0; i < cc.dropped.size(); ++i) {
            const dropped_replica &dr = cc.dropped[i];
            char time_buf[30];
            ::dsn::utils::time_ms_to_string(dr.time, time_buf);
            ddebug("%s: config_context.dropped[%d]: "
                   "node(%s), time(%" PRIu64 "){%s}, ballot(%" PRId64 "), "
                   "commit_decree(%" PRId64 "), prepare_decree(%" PRId64 ")",
                   gpid_name,
                   i,
                   dr.node.to_string(),
                   dr.time,
                   time_buf,
                   dr.ballot,
                   dr.last_committed_decree,
                   dr.last_prepared_decree);
        }

        for (int i = 0; i < pc.last_drops.size(); ++i) {
            ddebug("%s: config_context.last_drop[%d]: node(%s)",
                   gpid_name,
                   i,
                   pc.last_drops[i].to_string());
        }

        if (pc.last_drops.size() == 1) {
            dwarn("%s: the only node(%s) is dead, waiting it to come back",
                  gpid_name,
                  pc.last_drops.back().to_string());
            action.node = pc.last_drops.back();
        } else {
            std::vector<dsn::rpc_address> nodes(pc.last_drops.end() - 2, pc.last_drops.end());
            std::vector<dropped_replica> collected_info(2);
            bool ready = true;

            ddebug("%s: last two drops are %s and %s",
                   gpid_name,
                   nodes[0].to_string(),
                   nodes[1].to_string());

            for (unsigned int i = 0; i < nodes.size(); ++i) {
                node_state *ns = get_node_state(*view.nodes, nodes[i], false);
                if (ns == nullptr || !ns->alive()) {
                    dwarn("%s: last dropped node %s haven't come back yet",
                          gpid_name,
                          nodes[i].to_string());
                    ready = false;
                } else {
                    std::vector<dropped_replica>::iterator it = cc.find_from_dropped(nodes[i]);
                    if (it == cc.dropped.end() || it->ballot == invalid_ballot) {
                        if (ns->has_collected()) {
                            ddebug("%s: ignore %s's replica info as it doesn't exist on replica "
                                   "server",
                                   gpid_name,
                                   nodes[i].to_string());
                            collected_info[i] = {nodes[i], 0, -1, -1, -1};
                        } else {
                            ddebug("%s: last dropped node %s's info haven't been collected, "
                                   "reason(%s)",
                                   gpid_name,
                                   nodes[i].to_string(),
                                   it == cc.dropped.end() ? "item not exist in dropped"
                                                          : "no ballot/decree info");
                            ready = false;
                        }
                    } else {
                        collected_info[i] = *it;
                    }
                }
            }

            if (ready && collected_info[0].ballot == -1 && collected_info[1].ballot == -1) {
                ready = false;
                ddebug("%s: don't select primary as no info collected from non of last two drops",
                       gpid_name);
            }

            if (ready) {
                dropped_replica &previous_dead = collected_info[0];
                dropped_replica &recent_dead = collected_info[1];

                // 1. larger ballot should have larger committed decree
                // 2. max_prepared_decree should larger than meta's committed decree
                int64_t gap1 = previous_dead.ballot - recent_dead.ballot;
                int64_t gap2 =
                    previous_dead.last_committed_decree - recent_dead.last_committed_decree;
                if (gap1 * gap2 >= 0) {
                    int64_t larger_cd = std::max(previous_dead.last_committed_decree,
                                                 recent_dead.last_committed_decree);
                    int64_t larger_pd = std::max(previous_dead.last_prepared_decree,
                                                 recent_dead.last_prepared_decree);
                    if (larger_pd >= pc.last_committed_decree && larger_pd >= larger_cd) {
                        if (gap1 != 0) {
                            action.node = gap1 < 0 ? recent_dead.node : previous_dead.node;
                        } else if (gap2 != 0) {
                            action.node = gap2 < 0 ? recent_dead.node : previous_dead.node;
                        } else {
                            action.node = previous_dead.last_prepared_decree >
                                                  recent_dead.last_prepared_decree
                                              ? previous_dead.node
                                              : recent_dead.node;
                        }
                        ddebug(
                            "%s: select %s as a new primary", gpid_name, action.node.to_string());
                    } else {
                        ddebug("%s: don't select primary: larger_prepared_decree(%" PRId64 "), "
                               "last committed decree on meta(%" PRId64
                               "), larger_committed_decree(%" PRId64 ")",
                               gpid_name,
                               larger_pd,
                               pc.last_committed_decree,
                               larger_cd);
                    }
                } else {
                    ddebug("%s: don't select primary as the node with larger "
                           "ballot has smaller last prepared decree",
                           gpid_name);
                }
            }
        }

        if (!action.node.is_invalid()) {
            action.target = action.node;
            action.type = config_type::CT_ASSIGN_PRIMARY;
            action.period_ts = 86400;

            get_newly_partitions(*view.nodes, action.node)->newly_add_primary(gpid.get_app_id());
        } else {
            dwarn("%s: don't select any node for security reason, administrator can select "
                  "a proper one by shell",
                  gpid_name);
            _recent_choose_primary_fail_count.increment();
        }

        result = pc_status::dead;
    }

    if (action.type != config_type::CT_INVALID) {
        action.period_ts += (dsn_now_ms() / 1000);
        acts.assign_cure_proposal(action);
    }
    return result;
}

pc_status simple_load_balancer::on_missing_secondary(meta_view &view, const dsn::gpid &gpid)
{
    partition_configuration &pc = *get_config(*(view.apps), gpid);
    config_context &cc = *get_config_context(*(view.apps), gpid);
    proposal_actions &prop_acts = get_config_context(*(view.apps), gpid)->lb_actions;

    configuration_proposal_action action;
    bool is_emergency = false;
    if (replica_count(pc) < mutation_2pc_min_replica_count) {
        is_emergency = true;
        ddebug("gpid(%d.%d): is emergency due to too few replicas",
               gpid.get_app_id(),
               gpid.get_partition_index());
    } else if (cc.dropped.empty()) {
        is_emergency = true;
        ddebug("gpid(%d.%d): is emergency due to no dropped candidate",
               gpid.get_app_id(),
               gpid.get_partition_index());
    } else if (has_milliseconds_expired(cc.dropped.back().time +
                                        replica_assign_delay_ms_for_dropouts)) {
        is_emergency = true;
        char time_buf[30];
        ::dsn::utils::time_ms_to_string(cc.dropped.back().time, time_buf);
        ddebug("gpid(%d.%d): is emergency due to lose secondary for a long time, "
               "last_dropped_node(%s), drop_time(%s), delay_ms(%" PRIu64 ")",
               gpid.get_app_id(),
               gpid.get_partition_index(),
               cc.dropped.back().node.to_string(),
               time_buf,
               replica_assign_delay_ms_for_dropouts);
    }
    action.node.set_invalid();

    if (is_emergency) {
        std::ostringstream oss;
        for (int i = 0; i < cc.dropped.size(); ++i) {
            if (i != 0)
                oss << ",";
            oss << cc.dropped[i].node.to_string();
        }
        ddebug("gpid(%d.%d): try to choose node in dropped list, dropped_list(%s), "
               "prefered_dropped(%d)",
               gpid.get_app_id(),
               gpid.get_partition_index(),
               oss.str().c_str(),
               cc.prefered_dropped);
        if (cc.prefered_dropped < 0 || cc.prefered_dropped >= (int)cc.dropped.size()) {
            ddebug("gpid(%d.%d): prefered_dropped(%d) is invalid according to drop_list(size %d), "
                   "reset it to %d (drop_list.size - 1)",
                   gpid.get_app_id(),
                   gpid.get_partition_index(),
                   cc.prefered_dropped,
                   (int)cc.dropped.size(),
                   (int)cc.dropped.size() - 1);
            cc.prefered_dropped = (int)cc.dropped.size() - 1;
        }

        while (cc.prefered_dropped >= 0) {
            const dropped_replica &server = cc.dropped[cc.prefered_dropped];
            if (is_node_alive(*view.nodes, server.node)) {
                ddebug("gpid(%d.%d): node(%s) at cc.dropped[%d] is alive now, choose it, "
                       "and forward prefered_dropped from (%d) to (%d)",
                       gpid.get_app_id(),
                       gpid.get_partition_index(),
                       server.node.to_string(),
                       cc.prefered_dropped,
                       cc.prefered_dropped,
                       cc.prefered_dropped - 1);
                action.node = server.node;
                action.period_ts = suggest_alive_time(config_type::CT_ADD_SECONDARY);
                cc.prefered_dropped--;
                break;
            } else {
                ddebug("gpid(%d.%d): node(%s) at cc.dropped[%d] is not alive now, "
                       "changed prefered_dropped from (%d) to (%d)",
                       gpid.get_app_id(),
                       gpid.get_partition_index(),
                       server.node.to_string(),
                       cc.prefered_dropped,
                       cc.prefered_dropped,
                       cc.prefered_dropped - 1);
                cc.prefered_dropped--;
            }
        }

        if (action.node.is_invalid()) {
            newly_partitions *min_server_np = nullptr;
            for (auto &pairs : *view.nodes) {
                node_state &ns = pairs.second;
                if (!ns.alive() || is_member(pc, ns.addr()))
                    continue;
                newly_partitions *np = newly_partitions_ext::get_inited(&ns);
                if (min_server_np == nullptr ||
                    np->less_partitions(*min_server_np, gpid.get_app_id())) {
                    action.node = ns.addr();
                    min_server_np = np;
                }
            }

            if (!action.node.is_invalid()) {
                ddebug("gpid(%d.%d): can't find valid node in dropped list to add as secondary, "
                       "choose new node(%s) with minimal partitions serving",
                       gpid.get_app_id(),
                       gpid.get_partition_index(),
                       action.node.to_string());
                // need to copy data, so suggest more alive time
                action.period_ts = suggest_alive_time(config_type::CT_ADD_SECONDARY) * 2;
            } else {
                ddebug("gpid(%d.%d): can't find valid node in dropped list to add as secondary, "
                       "but also we can't find a new node to add as secondary",
                       gpid.get_app_id(),
                       gpid.get_partition_index());
            }
        }
    } else {
        // if not emergency, only try to recover last dropped server
        const dropped_replica &server = cc.dropped.back();
        if (is_node_alive(*view.nodes, server.node)) {
            dassert(!server.node.is_invalid(),
                    "invalid server address, address = %s",
                    server.node.to_string());
            action.node = server.node;
            action.period_ts = suggest_alive_time(config_type::CT_ADD_SECONDARY);
        }

        if (!action.node.is_invalid()) {
            ddebug("gpid(%d.%d): choose node(%s) as secondary coz it is last_dropped_node and is "
                   "alive now",
                   gpid.get_app_id(),
                   gpid.get_partition_index(),
                   server.node.to_string());
        } else {
            ddebug("gpid(%d.%d): can't add secondary coz last_dropped_node(%s) is not alive now, "
                   "ignore this as not in emergency",
                   gpid.get_app_id(),
                   gpid.get_partition_index(),
                   server.node.to_string());
        }
    }

    if (!action.node.is_invalid()) {
        action.type = config_type::CT_ADD_SECONDARY;
        action.target = pc.primary;

        newly_partitions *np = get_newly_partitions(*(view.nodes), action.node);
        dassert(np != nullptr, "");
        np->newly_add_partition(gpid.get_app_id());

        action.period_ts += (dsn_now_ms() / 1000);
        prop_acts.assign_cure_proposal(action);
    }

    return pc_status::ill;
}

pc_status simple_load_balancer::on_redundant_secondary(meta_view &view, const dsn::gpid &gpid)
{
    const node_mapper &nodes = *(view.nodes);
    const partition_configuration &pc = *get_config(*(view.apps), gpid);
    int target = 0;
    int load = nodes.find(pc.secondaries.front())->second.partition_count();
    for (int i = 0; i != pc.secondaries.size(); ++i) {
        int l = nodes.find(pc.secondaries[i])->second.partition_count();
        if (l > load) {
            load = l;
            target = i;
        }
    }

    configuration_proposal_action action;
    action.type = config_type::CT_REMOVE;
    action.node = pc.secondaries[target];
    action.target = pc.primary;
    action.period_ts = dsn_now_ms() / 1000 + suggest_alive_time(action.type);
    get_config_context(*view.apps, gpid)->lb_actions.assign_cure_proposal(action);
    return pc_status::ill;
}

pc_status simple_load_balancer::cure(meta_view view,
                                     const dsn::gpid &gpid,
                                     configuration_proposal_action &action)
{
    if (from_proposals(view, gpid, action))
        return pc_status::ill;

    std::shared_ptr<app_state> &app = (*view.apps)[gpid.get_app_id()];
    const partition_configuration &pc = *get_config(*(view.apps), gpid);
    const proposal_actions &acts = get_config_context(*view.apps, gpid)->lb_actions;

    dassert(app->is_stateful, "");
    dassert(acts.empty(), "");

    pc_status status;
    if (pc.primary.is_invalid())
        status = on_missing_primary(view, gpid);
    else if (static_cast<int>(pc.secondaries.size()) + 1 < pc.max_replica_count)
        status = on_missing_secondary(view, gpid);
    else if (static_cast<int>(pc.secondaries.size()) >= pc.max_replica_count)
        status = on_redundant_secondary(view, gpid);
    else
        status = pc_status::healthy;

    if (!acts.empty()) {
        action = acts.acts.front();
    }
    return status;
}

bool simple_load_balancer::collect_replica(meta_view view,
                                           const rpc_address &node,
                                           const replica_info &info)
{
    partition_configuration &pc = *get_config(*view.apps, info.pid);
    config_context &cc = *get_config_context(*view.apps, info.pid);
    if (is_member(pc, node)) {
        cc.collect_serving_replica(node, info);
        return true;
    }

    int ans = cc.collect_drop_replica(node, info);
    dassert(cc.check_order(), "");
    return ans != -1;
}

bool simple_load_balancer::construct_replica(meta_view view, const gpid &pid, int max_replica_count)
{
    partition_configuration &pc = *get_config(*view.apps, pid);
    config_context &cc = *get_config_context(*view.apps, pid);

    dassert(replica_count(pc) == 0,
            "replica count of gpid(%d.%d) must be 0",
            pid.get_app_id(),
            pid.get_partition_index());
    dassert(
        max_replica_count > 0, "max replica count is %d, should be at lease 1", max_replica_count);

    std::vector<dropped_replica> &drop_list = cc.dropped;
    if (drop_list.empty()) {
        dwarn("construct for (%d.%d) failed, coz no replicas collected",
              pid.get_app_id(),
              pid.get_partition_index());
        return false;
    }

    // treat last server in drop_list as the primary
    const dropped_replica &server = drop_list.back();
    dassert(server.ballot != invalid_ballot,
            "the ballot of server must not be invalid_ballot, node = %s",
            server.node.to_string());
    pc.primary = server.node;
    pc.ballot = server.ballot;
    pc.partition_flags = 0;
    pc.max_replica_count = max_replica_count;

    ddebug("construct for (%d.%d), select %s as primary, ballot(%" PRId64
           "), committed_decree(%" PRId64 "), prepare_decree(%" PRId64 ")",
           pid.get_app_id(),
           pid.get_partition_index(),
           server.node.to_string(),
           server.ballot,
           server.last_committed_decree,
           server.last_prepared_decree);

    drop_list.pop_back();

    // we put max_replica_count-1 recent replicas to last_drops, in case of the DDD-state when the
    // only primary dead
    // when add node to pc.last_drops, we don't remove it from our cc.drop_list
    dassert(pc.last_drops.empty(),
            "last_drops of partition(%d.%d) must be empty",
            pid.get_app_id(),
            pid.get_partition_index());
    for (auto iter = drop_list.rbegin(); iter != drop_list.rend(); ++iter) {
        if (pc.last_drops.size() + 1 >= max_replica_count)
            break;
        // similar to cc.drop_list, pc.last_drop is also a stack structure
        pc.last_drops.insert(pc.last_drops.begin(), iter->node);
        ddebug("construct for (%d.%d), select %s into last_drops, ballot(%" PRId64
               "), committed_decree(%" PRId64 "), prepare_decree(%" PRId64 ")",
               pid.get_app_id(),
               pid.get_partition_index(),
               iter->node.to_string(),
               iter->ballot,
               iter->last_committed_decree,
               iter->last_prepared_decree);
    }

    cc.prefered_dropped = (int)drop_list.size() - 1;
    return true;
}
}
}
