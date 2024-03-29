#include "../include/trail.hpp"

trail::trail(name self, name code, datastream<const char*> ds) : contract(self, code, ds) {}

trail::~trail() {}

//======================== admin actions ========================

ACTION trail::setconfig(string trail_version, bool set_defaults) {
    //authenticate
    require_auth(get_self());

    //initialize
    map<name, asset> new_fees;
    map<name, uint32_t> new_times;

    if (set_defaults) {
        //set default fees
        new_fees[name("ballot")] = asset(300000, TLOS_SYM); //30 TLOS
        new_fees[name("registry")] = asset(2500000, TLOS_SYM); //250 TLOS
        new_fees[name("archival")] = asset(50000, TLOS_SYM); //5 TLOS (per day)

        //set default times
        new_times[name("minballength")] = uint32_t(60); //1 day in seconds
        new_times[name("balcooldown")] = uint32_t(432000); //5 days in seconds
    }

    //open config singleton
    config_singleton configs(get_self(), get_self().value);

    //build new configs
    config new_config = {
        trail_version, //trail_version
        new_fees, //fees
        new_times //times
    };

    //set new config
    configs.set(new_config, get_self());
}

ACTION trail::updatefee(name fee_name, asset fee_amount) {
    //authenticate
    require_auth(get_self());

    //open configs singleton
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //validate
    check(fee_amount >= asset(0, TLOS_SYM), "fee amount must be a positive number");

    //initialize
    config new_conf = conf;

    //update fee
    new_conf.fees[fee_name] = fee_amount;

    //update fee
    configs.set(new_conf, get_self());
}

ACTION trail::updatetime(name time_name, uint32_t length) {
    //authenticate
    require_auth(get_self());

    //open configs singleton
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //validate
    check(length >= 1, "length must be a positive number");

    //initialize
    config new_conf = conf;

    //update time name with new length
    new_conf.times[time_name] = length;

    //update time
    configs.set(new_conf, get_self());
}

//======================== registry actions ========================

ACTION trail::newregistry(name manager, asset max_supply, name access) {
    //authenticate
    require_auth(manager);

    //open registries table, search for registry
    registries_table registries(get_self(), get_self().value);
    auto reg = registries.find(max_supply.symbol.code().raw());

    //open configs singleton, get configs
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //validate
    check(reg == registries.end(), "registry already exists");
    check(max_supply.amount > 0, "max supply must be greater than 0");
    check(max_supply.symbol.is_valid(), "invalid symbol name");
    check(max_supply.is_valid(), "invalid max supply");
    check(valid_access_method(access), "invalid access method");

    //check reserved symbols
    if (manager != name("eosio")) {
        check(max_supply.symbol.code().raw() != TLOS_SYM.code().raw(), "TLOS symbol is reserved");
        check(max_supply.symbol.code().raw() != VOTE_SYM.code().raw(), "VOTE symbol is reserved");
        check(max_supply.symbol.code().raw() != TRAIL_SYM.code().raw(), "TRAIL symbol is reserved");
    }

    //charge registry fee
    require_fee(manager, conf.fees.at(name("registry")));

    //set up initial settings
    map<name, bool> initial_settings;
    initial_settings[name("transferable")] = false;
    initial_settings[name("burnable")] = false;
    initial_settings[name("reclaimable")] = false;
    initial_settings[name("stakeable")] = false;
    initial_settings[name("unstakeable")] = false;
    initial_settings[name("maxmutable")] = false;

    //emplace new token registry, RAM paid by manager
    registries.emplace(manager, [&](auto& col) {
        col.supply = asset(0, max_supply.symbol);
        col.max_supply = max_supply;
        col.voters = uint32_t(0);
        col.access = access;
        col.locked = false;
        col.unlock_acct = manager;
        col.unlock_auth = name("active");
        col.manager = manager;
        col.settings = initial_settings;
        col.open_ballots = uint16_t(0);
        col.worker_funds = asset(0, TLOS_SYM);
        col.rebalanced_volume = asset(0, max_supply.symbol);
        col.rebalanced_count = uint32_t(0);
        col.cleaned_count = uint32_t(0);
    });

}

ACTION trail::togglereg(symbol registry_symbol, name setting_name) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);

    //validate
    check(!reg.locked, "registry is locked");
    auto set_itr = reg.settings.find(setting_name);
    check(set_itr != reg.settings.end(), "setting not found");

    //update setting
    registries.modify(reg, same_payer, [&](auto& col) {
        col.settings[setting_name] = !reg.settings.at(setting_name);
    });
}

ACTION trail::mint(name to, asset quantity, string memo) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(quantity.symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);

    //validate
    check(is_account(to), "to account doesn't exist");
    check(reg.supply + quantity <= reg.max_supply, "minting would breach max supply");
    check(quantity.amount > 0, "must mint a positive quantity");
    check(quantity.is_valid(), "invalid quantity");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    //update recipient liquid amount
    add_liquid(to, quantity);

    //update registry supply
    registries.modify(reg, same_payer, [&](auto& col) {
        col.supply += quantity;
    });

    //notify to account
    require_recipient(to);
}

ACTION trail::transfer(name from, name to, asset quantity, string memo) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(quantity.symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(from);

    //validate
    check(is_account(to), "to account doesn't exist");
    check(reg.settings.at(name("transferable")), "token is not transferable");
    check(from != to, "cannot transfer tokens to yourself");
    check(quantity.amount > 0, "must transfer positive quantity");
    check(quantity.is_valid(), "invalid quantity");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    //subtract quantity from sender liquid amount
    sub_liquid(from, quantity);

    //add quantity to recipient liquid amount
    add_liquid(to, quantity);

    //notify from and to accounts
    require_recipient(from);
    require_recipient(to);
}

ACTION trail::burn(asset quantity, string memo) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(quantity.symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);

    //open voters table, get manager
    voters_table voters(get_self(), reg.manager.value);
    auto& mgr = voters.get(quantity.symbol.code().raw(), "manager voter not found");

    //validate
    check(reg.settings.at(name("burnable")), "token is not burnable");
    check(reg.supply - quantity >= asset(0, quantity.symbol), "cannot burn supply below zero");
    check(mgr.liquid >= quantity, "burning would overdraw balance");
    check(quantity.amount > 0, "must burn a positive quantity");
    check(quantity.is_valid(), "invalid quantity");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    //subtract quantity from manager liquid amount
    sub_liquid(reg.manager, quantity);

    //update registry supply
    registries.modify(reg, same_payer, [&](auto& col) {
        col.supply -= quantity;
    });

    //notify manager account
    require_recipient(reg.manager);
}

ACTION trail::reclaim(name voter, asset quantity, string memo) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(quantity.symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);

    //validate
    check(reg.settings.at(name("reclaimable")), "token is not reclaimable");
    check(reg.manager != voter, "cannot reclaim tokens from yourself");
    check(is_account(voter), "voter account doesn't exist");
    check(quantity.is_valid(), "invalid amount");
    check(quantity.amount > 0, "must reclaim positive amount");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    //sub quantity from liquid
    sub_liquid(voter, quantity);

    //add quantity to manager balance
    add_liquid(reg.manager, quantity);

    //notify voter account
    require_recipient(voter);
}

ACTION trail::mutatemax(asset new_max_supply, string memo) {
    //get registries table, open registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(new_max_supply.symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);

    //validate
    check(reg.settings.at(name("maxmutable")), "max supply is not modifiable");
    check(new_max_supply.is_valid(), "invalid amount");
    check(new_max_supply.amount >= 0, "max supply cannot be below zero");
    check(new_max_supply >= reg.supply, "cannot lower max supply below current supply");
    check(memo.size() <= 256, "memo has more than 256 bytes");

    //update max supply
    registries.modify(reg, same_payer, [&](auto& col) {
        col.max_supply = new_max_supply;
    });
}

ACTION trail::setunlocker(symbol registry_symbol, name new_unlock_acct, name new_unlock_auth) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);

    //validate
    check(!reg.locked, "registry is locked");
    check(is_account(new_unlock_acct), "unlock account doesn't exist");

    //update unlock acct and auth
    registries.modify(reg, same_payer, [&](auto& col) {
        col.unlock_acct = new_unlock_acct;
        col.unlock_auth = new_unlock_auth;
    });
}

ACTION trail::lockreg(symbol registry_symbol) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(reg.manager);
    check(!reg.locked, "registry is already locked");

    //update lock
    registries.modify(reg, same_payer, [&](auto& col) {
        col.locked = true;
    });
}

ACTION trail::unlockreg(symbol registry_symbol) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(permission_level{reg.unlock_acct, reg.unlock_auth});

    //validate
    check(reg.locked, "registry is already unlocked");

    //update lock
    registries.modify(reg, same_payer, [&](auto& col) {
        col.locked = false;
    });
}

ACTION trail::addtofund(symbol registry_symbol, name voter, asset quantity) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //charge quantity to account
    require_fee(voter, quantity);

    //debit quantity to registry worker fund
    registries.modify(reg, same_payer, [&](auto& col) {
        col.worker_funds += quantity;
    });
}

//======================== ballot actions ========================

ACTION trail::newballot(name ballot_name, name category, name publisher,  
    symbol registry_symbol, name voting_method, vector<name> initial_options) {
    //authenticate
    require_auth(publisher);

    //open configs singleton, get config
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //open voters table, get voter
    voters_table voters(get_self(), publisher.value);
    auto& vtr = voters.get(registry_symbol.code().raw(), "voter not found");

    //open ballots table
    ballots_table ballots(get_self(), get_self().value);
    auto bal = ballots.find(ballot_name.value);

    //charge ballot listing fee to publisher
    require_fee(publisher, conf.fees.at(name("ballot")));

    //validate
    check(bal == ballots.end(), "ballot name already exists");
    check(valid_category(category), "invalid category");
    check(valid_voting_method(voting_method), "invalid voting method");

    //create initial options map, initial settings map
    map<name, asset> new_initial_options;
    map<name, bool> new_settings;

    //loop and assign initial options
    //NOTE: duplicates are OK, they will be consolidated into 1 key anyway
    for (name n : initial_options) {
        new_initial_options[n] = asset(0, registry_symbol);
    }

    //intitial settings
    new_settings[name("lightballot")] = false;
    new_settings[name("revotable")] = true;
    new_settings[name("votestake")] = true;

    //emplace new ballot
    ballots.emplace(publisher, [&](auto& col){
        col.ballot_name = ballot_name;
        col.category = category;
        col.publisher = publisher;
        col.status = name("setup");
        col.title = "";
        col.description = "";
        col.ballot_info = "";
        col.voting_method = voting_method;
        col.max_options = 1;
        col.options = new_initial_options;
        col.registry_symbol = registry_symbol;
        col.total_voters = 0;
        col.settings = new_settings;
        col.cleaned_count = 0;
        col.begin_time = time_point_sec(0);
        col.end_time = time_point_sec(0);
    });
}

ACTION trail::editdetails(name ballot_name, string title, string description, string ballot_info) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //validate
    check(bal.status == name("setup"), "ballot must be in setup mode to edit details");

    //update ballot details
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.title = title;
        col.description = description;
        col.ballot_info = ballot_info;
    });
}

ACTION trail::togglebal(name ballot_name, name setting_name) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    auto set_itr = bal.settings.find(setting_name);

    //validate
    check(bal.status == name("setup"), "ballot must be in setup mode to toggle settings");
    check(set_itr != bal.settings.end(), "setting not found");

    //update ballot settings
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.settings[setting_name] = !bal.settings.at(setting_name);
    });
}

ACTION trail::editmaxopts(name ballot_name, uint8_t new_max_options) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //validate
    check(bal.status == name("setup"), "ballot must be in setup mode to edit max options");
    check(new_max_options > 0, "max options must be greater than zero");
    check(new_max_options <= bal.options.size(), "max options cannot be greater than number of options");

    //update ballot settings
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.max_options = new_max_options;
    });
}

ACTION trail::addoption(name ballot_name, name new_option_name) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //validate
    check(bal.status == name("setup"), "ballot must be in setup mode to add options");
    check(bal.options.find(new_option_name) == bal.options.end(), "option is already in ballot");

    ballots.modify(bal, same_payer, [&](auto& col) {
        col.options[new_option_name] = asset(0, bal.registry_symbol);
    });
}

ACTION trail::rmvoption(name ballot_name, name option_name) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //find option in ballot
    auto opt_itr = bal.options.find(option_name);

    //validate
    check(bal.status == name("setup"), "ballot must be in setup mode to remove options");
    check(opt_itr != bal.options.end(), "option not found");

    //erase ballot option
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.options.erase(opt_itr);
    });
}

ACTION trail::readyballot(name ballot_name, time_point_sec end_time) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //initialize
    auto now = time_point_sec(current_time_point());

    //open config singleton, get config
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(bal.registry_symbol.code().raw(), "registry not found");

    //update open ballots on registry
    registries.modify(reg, same_payer, [&](auto& col) {
        col.open_ballots += 1;
    });

    //validate
    check(bal.options.size() >= 2, "ballot must have at least 2 options");
    check(bal.status == name("setup"), "ballot must be in setup mode to ready");
    check(end_time.sec_since_epoch() - now.sec_since_epoch() >= conf.times.at(name("minballength")), "ballot must be open for minimum ballot length");
    check(end_time.sec_since_epoch() > now.sec_since_epoch(), "end time must be in the future");

    ballots.modify(bal, same_payer, [&](auto& col) {
        col.status = name("voting");
        col.begin_time = now;
        col.end_time = end_time;
    });
}

ACTION trail::cancelballot(name ballot_name, string memo) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //validate
    check(bal.status == name("voting"), "ballot must be in voting mode to cancel");

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(bal.registry_symbol.code().raw(), "registry not found");

    //update open ballots on registry
    registries.modify(reg, same_payer, [&](auto& col) {
        col.open_ballots -= 1;
    });

    //update ballot status
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.status = name("cancelled");
    });
}

ACTION trail::deleteballot(name ballot_name) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open configs singleton, get config
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //authenticate
    require_auth(bal.publisher);

    //initialize
    auto now = time_point_sec(current_time_point());

    //validate
    check(bal.status != name("voting"), "cannot delete while voting is in progress");
    check(bal.status != name("archived"), "cannot delete archived ballot");
    check(now > bal.end_time + conf.times.at(name("balcooldown")), "cannot delete until 5 days past ballot's end time");
    check(bal.cleaned_count == bal.total_voters, "must clean all ballot votes before deleting");

    //erase ballot
    ballots.erase(bal);
}

ACTION trail::postresults(name ballot_name, map<name, asset> light_results, uint32_t total_voters) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //validate
    check(bal.settings.at(name("lightballot")), "ballot is not a light ballot");
    check(bal.status == name("voting"), "ballot must be in voting mode to post results");
    check(bal.end_time < time_point_sec(current_time_point()), "must be past ballot end time to post");

    for (auto i = light_results.begin(); i != light_results.end(); i++) {
        check(i->second.symbol == bal.registry_symbol, "result has incorrect symbol");
    }
    
    //apply results to ballot
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.options = light_results;
        col.total_voters = total_voters;
        col.cleaned_count = total_voters;
    });
}

ACTION trail::closeballot(name ballot_name, bool broadcast) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //authenticate
    require_auth(bal.publisher);

    //validate
    check(bal.status == name("voting"), "ballot must be in voting mode to close");
    check(bal.end_time < time_point_sec(current_time_point()), "must be past ballot end time to close");

    //change ballot status
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.status = name("closed");
    });

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(bal.registry_symbol.code().raw(), "registry not found");

    //update open ballots on registry
    registries.modify(reg, same_payer, [&](auto& col) {
        col.open_ballots -= 1;
    });

    //perform 1tokensquare1v final sqrt()
    if (bal.voting_method == name("1tsquare1v") && !bal.settings.at(name("lightballot"))) {
        map<name, asset> squared_options = bal.options;

        //square root total votes on each option
        for (auto i = squared_options.begin(); i != squared_options.end(); i++) {
            squared_options[i->first] = asset(sqrtl(i->second.amount), bal.registry_symbol);
        }

        //update vote counts
        ballots.modify(bal, same_payer, [&](auto& col) {
            col.options = squared_options;
        });
    }

    //if broadcast true, send bcastresults inline to self
    if (broadcast) {
        action(permission_level{get_self(), name("active")}, get_self(), name("bcastresults"), make_tuple(
            ballot_name, //ballot_name
            bal.options, //final_results
            bal.total_voters //total_voters
        )).send();
    }
}

ACTION trail::bcastresults(name ballot_name, map<name, asset> final_results, uint32_t total_voters) {
    //authenticate
    //TODO: require_auth(permission_level{get_self(), name("postresults")});
    require_auth(get_self());

    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value);

    //initialize
    auto now = time_point_sec(current_time_point());

    //validate
    check(now > bal.end_time, "ballot voting must be complete before broadcasting results");

    //notify ballot publisher (for external contract processing)
    require_recipient(bal.publisher);
}

ACTION trail::archive(name ballot_name, time_point_sec archived_until) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open configs singleton, get config
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //open archivals table, search for archival
    archivals_table archivals(get_self(), get_self().value);
    auto arch = archivals.find(ballot_name.value);

    //authenticate
    require_auth(bal.publisher);

    //initialize
    auto now = time_point_sec(current_time_point()).sec_since_epoch();
    uint32_t days_to_archive = (archived_until.sec_since_epoch() - now) / uint32_t(86400) + 1;

    //validate
    check(arch == archivals.end(), "ballot is already archived");
    check(bal.status == name("closed"), "ballot must be closed to archive");
    check(archived_until.sec_since_epoch() > now, "archived until must be in the future");
    
    //calculate archive fee
    asset archival_fee = asset(conf.fees.at(name("archive")).amount * int64_t(days_to_archive), TLOS_SYM);

    //charge ballot publisher total fee
    require_fee(bal.publisher, archival_fee);

    //emplace in archived table
    archivals.emplace(bal.publisher, [&](auto& col) {
        col.ballot_name = ballot_name;
        col.archived_until = archived_until;
    });

    //change ballot status to archived
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.status = name("archived");
    });
}

ACTION trail::unarchive(name ballot_name, bool force) {
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open archivals table, get archival
    archivals_table archivals(get_self(), get_self().value);
    auto& arch = archivals.get(ballot_name.value, "archival not found");

    //initialize
    auto now = time_point_sec(current_time_point());

    //authenticate if force unarchive is true
    if (force) {
        //authenticate
        require_auth(bal.publisher);
    } else {
        //validate
        check(arch.archived_until < now, "ballot hasn't reached end of archival time");
    }

    //change ballot status to closed
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.status = name("closed");
    });

    //erase archival
    archivals.erase(arch);
}

//======================== voter actions ========================

ACTION trail::regvoter(name voter, symbol registry_symbol, optional<name> referrer) {
    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //open voters table, search for voter
    voters_table voters(get_self(), voter.value);
    auto vtr_itr = voters.find(registry_symbol.code().raw());

    //validate
    check(is_account(voter), "voter account doesn't exist");
    check(vtr_itr == voters.end(), "voter already exists");
    check(registry_symbol != TLOS_SYM, "cannot register as TLOS voter, use VOTE instead");
    check(registry_symbol != TRAIL_SYM, "cannot register as TRAIL voter, call regworker() instead");

    //initialize
    name ram_payer = voter;

    //authenticate
    switch (reg.access.value) {
        case (name("public").value):
            require_auth(voter);
            break;
        case (name("private").value):
            if (referrer) {
                name ref = *referrer;
                
                //authenticate
                require_auth(ref);

                //check referrer is registry manager
                check(ref == reg.manager, "referrer must be registry manager");

                //set referrer as ram payer
                ram_payer = ref;
            } else {
                require_auth(reg.manager);
            }
            break;
        case (name("invite").value):
            if (referrer) {
                name ref = *referrer;

                //authenticate
                require_auth(ref);

                //TODO: check referrer is a voter

                //set referrer as ram payer
                ram_payer = ref;
            } else {
                require_auth(reg.manager);
            }
            break;
        case (name("membership").value):
            //inline sent from trailservice@membership
            require_auth(permission_level{get_self(), name("membership")});
            ram_payer = get_self();
            //TODO: write membership payment features
            break;
        default:
            check(false, "invalid access method. contact registry manager.");
    }

    //emplace new voter
    voters.emplace(ram_payer, [&](auto& col) {
        col.liquid = asset(0, registry_symbol);
        col.staked = asset(0, registry_symbol);
    });

    //update registry
    registries.modify(reg, same_payer, [&](auto& col) {
        col.voters += 1;
    });
}

ACTION trail::unregvoter(name voter, symbol registry_symbol) {
    //authenticate
    require_auth(voter);

    //open voters table, get account
    voters_table voters(get_self(), voter.value);
    auto& vtr = voters.get(registry_symbol.code().raw(), "voter not found");

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //validate
    check(vtr.liquid == asset(0, registry_symbol), "cannot unregister unless liquid is zero");
    check(vtr.staked == asset(0, registry_symbol), "cannot unregister unless staked is zero");

    //TODO: let voter unregister anyway by sending liquid and staked amount?

    //TODO: require voter to cleanup/unvote all existing vote receipts first?

    registries.modify(reg, same_payer, [&](auto& col) {
        col.voters -= uint32_t(1);
    });

    //erase account
    voters.erase(vtr);
}

ACTION trail::castvote(name voter, name ballot_name, vector<name> options) {
    //authenticate
    require_auth(voter);

    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open voters table, get voter
    voters_table voters(get_self(), voter.value);
    auto& vtr = voters.get(bal.registry_symbol.code().raw(), "voter not found");

    //initialize acct
    auto now = time_point_sec(current_time_point());
    asset raw_vote_weight = asset(0, bal.registry_symbol);
    uint32_t new_voter = 1;
    map<name, asset> temp_bal_options = bal.options;

    if (bal.settings.at(name("votestake"))) { //use stake
        raw_vote_weight = vtr.staked;
    } else { //use balance
        raw_vote_weight = vtr.liquid;
    }

    //validate
    check(bal.status == name("voting"), "ballot status is must be in voting mode to cast vote");
    check(now >= bal.begin_time && now <= bal.end_time, "vote must occur between ballot begin and end times");
    check(options.size() <= bal.max_options, "cannot vote for more than ballot's max options");
    check(raw_vote_weight.amount > 0, "must vote with a positive amount");

    //skip vote tracking if light ballot
    if (bal.settings.at(name("lightballot"))) {
        return;
    }

    //open votereceipts table, search for existing vote receipt
    votes_table votes(get_self(), voter.value);
    auto v_itr = votes.find(ballot_name.value);

    //rollback if vote already exists
    if (v_itr != votes.end()) {
        
        //initialize
        auto v = *v_itr;

        //validate
        check(bal.settings.at(name("revotable")), "ballot is not revotable");

        //rollback if weighted votes are not empty
        if (!v.weighted_votes.empty()) {
            
            //rollback old votes
            for (auto i = v.weighted_votes.begin(); i != v.weighted_votes.end(); i++) {
                temp_bal_options[i->first] -= i->second;
            }

            //update new voter
            new_voter = 0;
        }
    }

    //calculate new votes
    auto new_votes = calc_vote_weights(bal.registry_symbol, bal.voting_method, options, raw_vote_weight);

    //apply new votes
    for (auto i = new_votes.begin(); i != new_votes.end(); i++) {
        
        //validate
        check(temp_bal_options.find(i->first) != temp_bal_options.end(), "option doesn't exist on ballot");

        //apply effective vote to ballot option
        temp_bal_options[i->first] += i->second;
    }

    //update ballot
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.options = temp_bal_options;
        col.total_voters += new_voter;
    });

    //update existing votes, or emplace votes if new
    if (new_voter == 1) {
        votes.emplace(voter, [&](auto& col) {
            col.ballot_name = ballot_name;
            col.registry_symbol = bal.registry_symbol;
            col.raw_vote_weight = raw_vote_weight;
            col.weighted_votes = new_votes;
            col.expiration = bal.end_time;
            col.worker = name(0);
            col.rebalances = uint16_t(0);
            col.rebalance_volume = asset(0, bal.registry_symbol);
        });
    } else {
        //update votes
        votes.modify(v_itr, same_payer, [&](auto& col) {
            col.raw_vote_weight = raw_vote_weight;
            col.weighted_votes = new_votes;
        });
    }

}

ACTION trail::unvoteall(name voter, name ballot_name) {
    //authenticate
    require_auth(voter);

    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open voters table, get voter
    voters_table voters(get_self(), voter.value);
    auto& vtr = voters.get(bal.registry_symbol.code().raw(), "voter not found");

    //open votes table, get vote
    votes_table votes(get_self(), voter.value);
    auto& v = votes.get(ballot_name.value, "vote not found");

    //initialize
    map<name, asset> temp_bal_options = bal.options;
    auto now = time_point_sec(current_time_point());

    //validate
    check(bal.status == name("voting"), "ballot must be in voting mode to unvote");
    check(now >= bal.begin_time && now <= bal.end_time, "must unvote between begin and end time");
    check(!v.weighted_votes.empty(), "votes are already empty");

    //return if light ballot
    if (bal.settings.at(name("lightballot"))) {
        return;
    }

    //rollback old votes
    for (auto i = v.weighted_votes.begin(); i != v.weighted_votes.end(); i++) {
        temp_bal_options[i->first] -= i->second;
    }

    //update ballot
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.options = temp_bal_options;
        col.total_voters -= 1;
    });

    //clear all votes in map (preserves rebalance count)
    votes.modify(v, same_payer, [&](auto& col) {
        col.weighted_votes.clear();
    });
    
}

ACTION trail::stake(name voter, asset quantity) {
    //authenticate
    require_auth(voter);

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(quantity.symbol.code().raw(), "registry not found");

    //validate
    check(reg.settings.at(name("stakeable")), "token is not stakeable");
    check(is_account(voter), "voter account doesn't exist");
    check(quantity.is_valid(), "invalid amount");
    check(quantity.amount > 0, "must stake positive amount");

    //subtract quantity from liquid
    sub_liquid(voter, quantity);

    //add quantity to staked
    add_stake(voter, quantity);
}

ACTION trail::unstake(name voter, asset quantity) {
    //authenticate
    require_auth(voter);

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(quantity.symbol.code().raw(), "registry not found");

    //validate
    check(reg.settings.at(name("unstakeable")), "token is not unstakeable");
    check(is_account(voter), "voter account doesn't exist");
    check(quantity.is_valid(), "invalid amount");
    check(quantity.amount > 0, "must unstake positive amount");

    //subtract quantity from stake
    sub_stake(voter, quantity);

    //add quantity to liquid
    add_liquid(voter, quantity);
}

//======================== worker actions ========================

ACTION trail::regworker(name worker_name) {
    //authenticate
    require_auth(worker_name);

    //open workers table, get worker
    workers_table workers(get_self(), get_self().value);
    auto wrk = workers.find(worker_name.value);

    //validate
    check(wrk == workers.end(), "worker already registered");

    //initialize
    auto now = time_point_sec(current_time_point());
    map<symbol, asset> new_rebalance_volume;
    map<symbol, uint16_t> new_rebalance_count;
    map<symbol, uint16_t> new_clean_count;

    //register new worker
    workers.emplace(worker_name, [&](auto& col) {
        col.worker_name = worker_name;
        col.standing = name("good");
        col.last_payment = now;
        col.rebalance_volume = new_rebalance_volume;
        col.rebalance_count = new_rebalance_count;
        col.clean_count = new_clean_count;
    });

    //open accounts table, search for account
    accounts_table tlos_accounts(get_self(), worker_name.value);
    auto acct = tlos_accounts.find(TLOS_SYM.code().raw());

    //emplace new account if not exists
    if (acct == tlos_accounts.end()) {
        tlos_accounts.emplace(worker_name, [&](auto& col) {
            col.balance = asset(0, TLOS_SYM);
        });
    }

    //open voters table, search for TRAIL voter
    voters_table trail_voters(get_self(), worker_name.value);
    auto vtr = trail_voters.find(TRAIL_SYM.code().raw());

    //emplace new TRAIL voter if not exists
    if (vtr == trail_voters.end()) {
        trail_voters.emplace(worker_name, [&](auto& col) {
            col.liquid = asset(0, TRAIL_SYM);
            col.staked = asset(0, TRAIL_SYM);
        });
    }

}

ACTION trail::unregworker(name worker_name) {
    //open workers table, get worker
    workers_table workers(get_self(), get_self().value);
    auto& wrk = workers.get(worker_name.value, "worker not found");

    //authenticate
    require_auth(wrk.worker_name);

    //validate
    // no validations necessary

    //NOTE: worker forfeits all current claim to rewards upon unregistration

    //erase worker
    workers.erase(wrk);
}

ACTION trail::claimpayment(name worker_name, symbol registry_symbol) {
    //open workers table, get worker
    workers_table workers(get_self(), get_self().value);
    auto& wrk = workers.get(worker_name.value);

    //open registries table, get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(registry_symbol.code().raw(), "registry not found");

    //authenticate
    require_auth(wrk.worker_name);

    //initialize
    asset total_tlos_payout = asset(0, TLOS_SYM);
    asset total_trail_payout = asset(0, TRAIL_SYM);
    auto now = time_point_sec(current_time_point()).sec_since_epoch();

    //validate
    check(wrk.last_payment.sec_since_epoch() + 86400 > now, "can only claim payment once per day");
    check(wrk.standing == name("good"), "must be in good standing to claim payment");

    //diminish rate: 1% of payout per day without claiming
    double reduced_by = ( (now - wrk.last_payment.sec_since_epoch() / 86400) - 1 ) / 100;

    //calculate worker payout
    asset pay_bucket = reg.worker_funds;
    asset payout = asset(0, TLOS_SYM);

    double vol_share = double(wrk.rebalance_volume.at(registry_symbol).amount) / double(reg.rebalanced_volume.amount);
    double count_share = double(wrk.rebalance_count.at(registry_symbol)) / double(reg.rebalanced_count);
    double clean_share = double(wrk.clean_count.at(registry_symbol)) / double(reg.cleaned_count);

    double total_share = (vol_share + count_share + clean_share) / double(3.0);
    payout = asset(int64_t(pay_bucket.amount * total_share), TLOS_SYM);

    //validate
    check(payout.amount > 0, "payout is zero. perform more work to increase payout share.");
    check(payout <= pay_bucket, "payout is more than worker funds. contact registry manager.");

    //update registry
    registries.modify(reg, same_payer, [&](auto& col) {
        col.worker_funds -= payout;
        col.rebalanced_volume -= wrk.rebalance_volume.at(registry_symbol);
        col.rebalanced_count -= wrk.rebalance_count.at(registry_symbol);
        col.cleaned_count -= wrk.clean_count.at(registry_symbol);
    });

    //reset worker
    workers.modify(wrk, same_payer, [&](auto& col) {
        col.last_payment = time_point_sec(current_time_point());
        col.rebalance_volume[registry_symbol] = asset(0, registry_symbol);
        col.rebalance_count[registry_symbol] = uint16_t(0);
        col.clean_count[registry_symbol] = uint16_t(0);
    });

    //open accounts table, get account
    accounts_table tlos_accounts(get_self(), worker_name.value);
    auto& tlos_acct = tlos_accounts.get(TLOS_SYM.code().raw(), "tlos account not found");

    //update worker tlos balance
    tlos_accounts.modify(tlos_acct, same_payer, [&](auto& col) {
        col.balance += payout;
    });

}

ACTION trail::rebalance(name voter, name ballot_name, optional<name> worker) {
    
    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open voters table, get voter
    voters_table voters(get_self(), voter.value);
    auto& vtr = voters.get(bal.registry_symbol.code().raw(), "voter not found");

    //open votes table, get vote
    votes_table votes(get_self(), voter.value);
    auto& v = votes.get(ballot_name.value, "vote not found");

    //initialize
    auto now = time_point_sec(current_time_point());
    asset raw_vote_weight = asset(0, v.registry_symbol);
    map<name, asset> new_bal_options = bal.options;
    vector<name> selections;
    name worker_name = name(0);

    //validate
    check(now < v.expiration, "vote receipt has expired");
    check(v.registry_symbol == bal.registry_symbol, "vote/ballot symbol mismatch");

    if (bal.settings.at(name("votestake"))) { //use stake
        raw_vote_weight = vtr.staked;
    } else { //use liquid
        raw_vote_weight = vtr.liquid;
    }

    //rollback old vote
    for (auto i = v.weighted_votes.begin(); i != v.weighted_votes.end(); i++) {
        new_bal_options[i->first] -= i->second;

        //rebuild selections
        selections.push_back(i->first);
    }

    //validate
    check(raw_vote_weight != v.raw_vote_weight, "vote is already balanced");
    check(selections.size() > 0, "cannot rebalance nonexistent votes");
    check(raw_vote_weight.amount > 0, "cannot vote with zero weight");

    //calculate new votes
    auto new_votes = calc_vote_weights(bal.registry_symbol, bal.voting_method, selections, raw_vote_weight);
    int64_t weight_delta = abs(v.raw_vote_weight.amount - raw_vote_weight.amount);

    //apply new votes to ballot
    for (auto i = new_votes.begin(); i != new_votes.end(); i++) {
        new_bal_options[i->first] += i->second;
    }

    //update ballot
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.options = new_bal_options;
    });

    //set worker info if applicable
    if (worker) {
        //initialize
        worker_name = *worker;

        //authenticate
        require_auth(worker_name);
    }

    //update vote
    votes.modify(v, same_payer, [&](auto& col) {
        col.raw_vote_weight = raw_vote_weight;
        col.weighted_votes = new_votes;
        col.worker = worker_name;
        col.rebalances += 1;
        col.rebalance_volume = asset(weight_delta, bal.registry_symbol);
    });

}

ACTION trail::cleanupvote(name voter, name ballot_name, optional<name> worker) {
    //open votes table, get vote
    votes_table votes(get_self(), voter.value);
    auto& v = votes.get(ballot_name.value, "vote not found");

    //open ballots table, get ballot
    ballots_table ballots(get_self(), get_self().value);
    auto& bal = ballots.get(ballot_name.value, "ballot not found");

    //open registries table get registry
    registries_table registries(get_self(), get_self().value);
    auto& reg = registries.get(bal.registry_symbol.code().raw(), "registry not found"); 

    //initialize
    auto now = time_point_sec(current_time_point());

    //validate
    check(v.expiration < now, "vote hasn't expired");
    
    //update ballot
    ballots.modify(bal, same_payer, [&](auto& col) {
        col.cleaned_count += 1;
    });

    //update registry
    registries.modify(reg, same_payer, [&](auto& col) {
        col.rebalanced_volume += v.rebalance_volume;
        col.rebalanced_count += uint32_t(v.rebalances);
        col.cleaned_count += uint32_t(1);
    });

    //log worker share if worker
    if (worker) {
        name worker_name = *worker;

        //authenticate
        require_auth(worker_name);
        
        //update cleanup worker
        log_cleanup_work(worker_name, v.registry_symbol, 1);
    }

    //update rebalance worker
    if (v.worker != name(0)) {
        log_rebalance_work(v.worker, v.registry_symbol, v.rebalance_volume, v.rebalances);
    }

    //erase expired vote
    votes.erase(v);
    
}

ACTION trail::withdraw(name voter, asset quantity) {
    
    //authenticate
    require_auth(voter);
    
    //open accounts table, get account
    accounts_table tlos_accounts(get_self(), voter.value);
    auto& acct = tlos_accounts.get(TLOS_SYM.code().raw(), "account not found");

    //validate
    check(quantity.symbol == TLOS_SYM, "can only withdraw TLOS");
    check(acct.balance >= quantity, "insufficient balance");
    check(quantity > asset(0, TLOS_SYM), "must withdraw a positive amount");

    //update balances
    tlos_accounts.modify(acct, same_payer, [&](auto& col) {
        col.balance -= quantity;
    });

    //transfer to eosio.token
    //inline trx requires trailservice@active to have trailservice@eosio.code
    action(permission_level{get_self(), name("active")}, name("eosio.token"), name("transfer"), make_tuple(
		get_self(), //from
		voter, //to
		quantity, //quantity
        std::string("trailservice withdrawal") //memo
	)).send();

}

//======================== committee actions ========================

ACTION trail::regcommittee(name committee_name, string committee_title,
    symbol registry_symbol, vector<name> initial_seats, name registree) {
    //authenticate
    require_auth(registree);

    //open committees table, search for committee
    committees_table committees(get_self(), registry_symbol.code().raw());
    auto cmt = committees.find(committee_name.value);

    //open voters table, get voter
    voters_table voters(get_self(), registree.value);
    auto& vtr = voters.get(registry_symbol.code().raw(), "voter not found");

    //open configs singleton, get config
    config_singleton configs(get_self(), get_self().value);
    auto conf = configs.get();

    //initialize
    map<name, name> new_seats;

    //validate
    check(cmt == committees.end(), "committtee already exists");
    check(committee_title.size() <= 256, "committee title has more than 256 bytes");

    //charge committee fee
    require_fee(registree, conf.fees.at(name("committee")));

    for (name n : initial_seats) {
        check(new_seats.find(n) == new_seats.end(), "seat names must be unique");
        new_seats[n] = name(0);
    }

    //create committee
    committees.emplace(registree, [&](auto& col) {
        col.committee_title = committee_title;
        col.committee_name = committee_name;
        col.registry_symbol = registry_symbol;
        col.seats = new_seats;
        col.updater_acct = registree;
        col.updater_auth = name("active");
    });
}

ACTION trail::addseat(name committee_name, symbol registry_symbol, name new_seat_name) {
    //open committees table, get committee
    committees_table committees(get_self(), registry_symbol.code().raw());
    auto& cmt = committees.get(committee_name.value, "committee not found");
    
    //authenticate
    require_auth(permission_level{cmt.updater_acct, cmt.updater_auth});

    //validate
    check(cmt.seats.find(new_seat_name) == cmt.seats.end(), "seat already exists");

    //add seat to committee
    committees.modify(cmt, same_payer, [&](auto& col) {
        col.seats[new_seat_name] = name(0);
    });
}

ACTION trail::removeseat(name committee_name, symbol registry_symbol, name seat_name) {
    //open committees table, get committee
    committees_table committees(get_self(), registry_symbol.code().raw());
    auto& cmt = committees.get(committee_name.value, "committee not found");
    
    //authenticate
    require_auth(permission_level{cmt.updater_acct, cmt.updater_auth});

    //validate
    check(cmt.seats.find(seat_name) != cmt.seats.end(), "seat name not found");

    //remove seat from committee
    committees.modify(cmt, same_payer, [&](auto& col) {
        col.seats.erase(seat_name);
    });
}

ACTION trail::assignseat(name committee_name, symbol registry_symbol, name seat_name, name seat_holder, string memo) {
    //open committees table, get committee
    committees_table committees(get_self(), registry_symbol.code().raw());
    auto& cmt = committees.get(committee_name.value, "committee not found");
    
    //authenticate
    require_auth(permission_level{cmt.updater_acct, cmt.updater_auth});

    //validate
    check(cmt.seats.find(seat_name) != cmt.seats.end(), "seat name not found");

    //assign seat holder to seat on committee
    committees.modify(cmt, same_payer, [&](auto& col) {
        col.seats[seat_name] = seat_holder;
    });
}

ACTION trail::setupdater(name committee_name, symbol registry_symbol, name updater_account, name updater_auth) {
    //open committees table, get committee
    committees_table committees(get_self(), registry_symbol.code().raw());
    auto& cmt = committees.get(committee_name.value, "committee not found");
    
    //authenticate
    require_auth(permission_level{cmt.updater_acct, cmt.updater_auth});

    //set new committee updater account and updater auth
    committees.modify(cmt, same_payer, [&](auto& col) {
        col.updater_acct = updater_account;
        col.updater_auth = updater_auth;
    });
}

ACTION trail::delcommittee(name committee_name, symbol registry_symbol, string memo) {
    //open committees table, get committee
    committees_table committees(get_self(), registry_symbol.code().raw());
    auto& cmt = committees.get(committee_name.value, "committee not found");
    
    //authenticate
    require_auth(permission_level{cmt.updater_acct, cmt.updater_auth});

    //erase committee
    committees.erase(cmt);
}

//========== notification methods ==========

void trail::catch_delegatebw(name from, name receiver, asset stake_net_quantity, asset stake_cpu_quantity, bool transfer) {
    //sync external stake with internal stake
    sync_external_account(from, VOTE_SYM, stake_net_quantity.symbol);
}

void trail::catch_undelegatebw(name from, name receiver, asset unstake_net_quantity, asset unstake_cpu_quantity) {
    //sync external stake with internal stake
    sync_external_account(from, VOTE_SYM, unstake_net_quantity.symbol);
}

void trail::catch_transfer(name from, name to, asset quantity, string memo) {
    //get initial receiver contract
    name rec = get_first_receiver();

    //validate
    if (rec == name("eosio.token") && from != get_self() && quantity.symbol == TLOS_SYM) {
        
        //parse memo
        if (memo == std::string("skip")) { //allows transfers to trail without triggering reaction
            return;
        } else if (memo == "deposit") {

            //open accounts tbale, search for account
            accounts_table accounts(get_self(), from.value);
            auto acct = accounts.find(TLOS_SYM.code().raw());

            //empalce account if not found, update if exists
            if (acct == accounts.end()) { //no account
                accounts.emplace(get_self(), [&](auto& col) {
                    col.balance = quantity;
                });
            } else { //exists
                accounts.modify(*acct, same_payer, [&](auto& col) {
                    col.balance += quantity;
                });
            }
        }
    }

}

//========== utility methods ==========

void trail::add_liquid(name voter, asset quantity) {
    //open voters table, get voter
    voters_table to_voters(get_self(), voter.value);
    auto& to_voter = to_voters.get(quantity.symbol.code().raw(), "add_liquid: voter not found");

    //add quantity to liquid
    to_voters.modify(to_voter, same_payer, [&](auto& col) {
        col.liquid += quantity;
    });
}

void trail::sub_liquid(name voter, asset quantity) {
    //open voters table, get voter
    voters_table from_voters(get_self(), voter.value);
    auto& from_voter = from_voters.get(quantity.symbol.code().raw(), "sub_liquid: voter not found");

    //validate
    check(from_voter.liquid >= quantity, "insufficient liquid amount");

    //subtract quantity from liquid
    from_voters.modify(from_voter, same_payer, [&](auto& col) {
        col.liquid -= quantity;
    });
}

void trail::add_stake(name voter, asset quantity) {
    //open voters table, get voter
    voters_table to_voters(get_self(), voter.value);
    auto& to_voter = to_voters.get(quantity.symbol.code().raw(), "add_stake: voter not found");

    //add quantity to stake
    to_voters.modify(to_voter, same_payer, [&](auto& col) {
        col.staked += quantity;
    });
}

void trail::sub_stake(name voter, asset quantity) {
    //open voters table, get voter
    voters_table from_voters(get_self(), voter.value);
    auto& from_voter = from_voters.get(quantity.symbol.code().raw(), "sub_stake: voter not found");

    //validate
    check(from_voter.staked >= quantity, "insufficient staked amount");

    //subtract quantity from stake
    from_voters.modify(from_voter, same_payer, [&](auto& col) {
        col.staked -= quantity;
    });
}

bool trail::valid_category(name category) {
    switch (category.value) {
        case (name("proposal").value):
            return true;
        case (name("referendum").value):
            return true;
        case (name("election").value):
            return true;
        case (name("poll").value):
            return true;
        case (name("leaderboard").value):
            return true;
        default:
            return false;
    }
}

bool trail::valid_voting_method(name voting_method) {
    switch (voting_method.value) {
        case (name("1acct1vote").value):
            return true;
        case (name("1tokennvote").value):
            return true;
        case (name("1token1vote").value):
            return true;
        case (name("1tsquare1v").value):
            return true;
        case (name("quadratic").value):
            return true;
        case (name("ranked").value):
            check(false, "ranked voitng method feature under development");
            return true;
        default:
            return false;
    }
}

bool trail::valid_access_method(name access_method) {
    switch (access_method.value) {
        case (name("public").value):
            return true;
        case (name("private").value):
            return true;
        case (name("invite").value):
            return true;
        case (name("membership").value):
            check(false, "membership access feature under development");
            return true;
        default:
            return false;
    }
}

void trail::require_fee(name account_name, asset fee) {
    //open accounts table, get TLOS balance
    accounts_table tlos_accounts(get_self(), account_name.value);
    auto& tlos_acct = tlos_accounts.get(TLOS_SYM.code().raw(), "TLOS balance not found");

    //validate
    check(tlos_acct.balance >= fee, "insufficient funds to cover fee");

    //charge fee
    tlos_accounts.modify(tlos_acct, same_payer, [&](auto& col) {
        col.balance -= fee;
    });
}

void trail::log_rebalance_work(name worker, symbol registry_symbol, asset volume, uint16_t count) {
    //open workers table, get worker
    workers_table workers(get_self(), get_self().value);
    auto w = workers.find(worker.value);

    if (w != workers.end()) {

        //initialize
        auto& wrk = *w;

        if (wrk.rebalance_volume.find(registry_symbol) == wrk.rebalance_volume.end()) {

            auto new_rebalance_volume = wrk.rebalance_volume;
            new_rebalance_volume[registry_symbol] = volume;

            auto new_rebalance_count = wrk.rebalance_count;
            new_rebalance_count[registry_symbol] = count;

            workers.modify(wrk, same_payer, [&](auto& col) {
                col.rebalance_volume = new_rebalance_volume;
                col.rebalance_count = new_rebalance_count;
            });

        } else {

            workers.modify(wrk, same_payer, [&](auto& col) {
                col.rebalance_volume[registry_symbol] += volume;
                col.rebalance_count[registry_symbol] += count;
            });

        }
        
    }
    
}

void trail::log_cleanup_work(name worker, symbol registry_symbol, uint16_t count) {
    //open workers table, get worker
    workers_table workers(get_self(), get_self().value);
    auto w = workers.find(worker.value);

    if (w != workers.end()) {

        //initialize
        auto& wrk = *w;

        if (wrk.clean_count.find(registry_symbol) == wrk.clean_count.end()) {

            auto new_clean_count = wrk.clean_count;
            new_clean_count[registry_symbol] = count;

            workers.modify(wrk, same_payer, [&](auto& col) {
                col.clean_count = new_clean_count;
            });

        } else {

            workers.modify(wrk, same_payer, [&](auto& col) {
                col.clean_count[registry_symbol] += count;
            });

        }

    }

}

void trail::sync_external_account(name voter, symbol internal_symbol, symbol external_symbol) {
    
    //initialize
    asset tlos_stake;

    //check external symbol is TLOS, if not then return
    if (external_symbol == TLOS_SYM) {
        tlos_stake = get_staked_tlos(voter);
        check(internal_symbol == VOTE_SYM, "internal symbol must be VOTE for external TLOS");
    } else {
        check(false, "syncing external accounts is under development");
        return;
    }

    //open voters table, search for voter
    voters_table voters(get_self(), voter.value);
    auto vtr_itr = voters.find(internal_symbol.code().raw());

    //subtract from VOTE stake
    if (vtr_itr != voters.end()) { //voter exists

        //open registries table, search for registry
        registries_table registries(get_self(), get_self().value);
        auto reg_itr = registries.find(internal_symbol.code().raw());

        //check if registry exists (should always be true)
        if (reg_itr != registries.end()) {
            
            //calc delta
            asset delta = asset(tlos_stake.amount - vtr_itr->staked.amount, internal_symbol);

            //apply delta to supply
            registries.modify(*reg_itr, same_payer, [&](auto& col) {
                col.supply += asset(delta.amount, internal_symbol);
            });

        } else {
            return;
        }

        //mirror tlos_stake to internal_symbol stake
        voters.modify(*vtr_itr, same_payer, [&](auto& col) {
            col.staked = asset(tlos_stake.amount, internal_symbol);
        });

    }

}

map<name, asset> trail::calc_vote_weights(symbol registry_symbol, name voting_method, 
    vector<name> selections,  asset raw_vote_weight) {
    
    //initialize
    map<name, asset> vote_weights;
    int64_t effective_amount;
    int64_t vote_amount_per;
    uint8_t sym_prec = registry_symbol.precision();
    int8_t pos = 1;

    switch (voting_method.value) {
        case (name("1acct1vote").value):
            effective_amount = int64_t(pow(10, sym_prec));
            break;
        case (name("1tokennvote").value):
            effective_amount = raw_vote_weight.amount;
            break;
        case (name("1token1vote").value):
            effective_amount = raw_vote_weight.amount / selections.size();
            break;
        case (name("1tsquare1v").value):
            vote_amount_per = raw_vote_weight.amount / selections.size();
            effective_amount = vote_amount_per * vote_amount_per;
            break;
        case (name("quadratic").value):
            effective_amount = sqrtl(raw_vote_weight.amount);
            break;
        case (name("ranked").value):
            //NOTE: requires selections to always be in order
            for (name n : selections) {
                effective_amount = raw_vote_weight.amount / pos;
                vote_weights[n] = asset(effective_amount, registry_symbol);
                pos++;
            }
            return vote_weights;
        default:
            check(false, "calc_vote_weights: invalid voting method");
    }

    //apply effective weight to vote mapping
    for (name n : selections) {
        vote_weights[n] = asset(effective_amount, registry_symbol);
    }

    return vote_weights;
}
