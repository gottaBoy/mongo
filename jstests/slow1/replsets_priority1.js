// come up with random priorities and make sure that the right member gets
// elected. then kill that member and make sure the next one gets elected.

(function() {

"use strict";

// Skip this test if running with --nojournal and WiredTiger.
if (jsTest.options().noJournal &&
    (!jsTest.options().storageEngine || jsTest.options().storageEngine === "wiredTiger")) {
    print("Skipping test because running WiredTiger without journaling isn't a valid" +
          " replica set configuration");
    return;
}

load("jstests/replsets/rslib.js");

var rs = new ReplSetTest({name: 'testSet', nodes: 3, nodeOptions: {verbose: 2}});
var nodes = rs.startSet();
rs.initiate();

var master = rs.getPrimary();

var everyoneOkSoon = function() {
    var status;
    assert.soon(function() {
        var ok = true;
        status = master.adminCommand({replSetGetStatus: 1});

        if (!status.members) {
            return false;
        }

        for (var i in status.members) {
            if (status.members[i].health == 0) {
                continue;
            }
            ok &= status.members[i].state == 1 || status.members[i].state == 2;
        }
        return ok;
    }, tojson(status));
};

var checkPrimaryIs = function(node) {
    print("nreplsets_priority1.js checkPrimaryIs(" + node.host + ")");

    var status;

    assert.soon(function() {
        var ok = true;

        try {
            status = master.adminCommand({replSetGetStatus: 1});
        } catch (e) {
            print(e);
            print("nreplsets_priority1.js checkPrimaryIs reconnecting");
            reconnect(master);
            status = master.adminCommand({replSetGetStatus: 1});
        }

        var str = "goal: " + node.host + "==1 states: ";
        if (!status || !status.members) {
            return false;
        }
        status.members.forEach(function(m) {
            str += m.name + ": " + m.state + " ";

            if (m.name == node.host) {
                ok &= m.state == 1;
            } else {
                ok &= m.state != 1 || (m.state == 1 && m.health == 0);
            }
        });
        print();
        print(str);
        print();

        occasionally(function() {
            print("\nstatus:");
            printjson(status);
            print();
        }, 15);

        return ok;
    }, node.host + '==1', 240000, 1000);

    everyoneOkSoon();
};

everyoneOkSoon();

jsTestLog("replsets_priority1.js initial sync");

// intial sync
master.getDB("foo").bar.insert({x: 1});
rs.awaitReplication();

jsTestLog("replsets_priority1.js starting loop");

var n = 5;
for (var i = 0; i < n; i++) {
    jsTestLog("Round " + i + ": FIGHT!");

    var max = null;
    var second = null;
    master = rs.getPrimary();
    var config = master.getDB("local").system.replset.findOne();

    var version = config.version;
    config.version++;

    for (var j = 0; j < config.members.length; j++) {
        var priority = Math.random() * 100;
        print("random priority : " + priority);
        config.members[j].priority = priority;

        if (!max || priority > max.priority) {
            max = config.members[j];
        }
    }

    for (var j = 0; j < config.members.length; j++) {
        if (config.members[j] == max) {
            continue;
        }
        if (!second || config.members[j].priority > second.priority) {
            second = config.members[j];
        }
    }

    jsTestLog("replsets_priority1.js max is " + max.host + " with priority " + max.priority +
              ", reconfiguring on " + master.host);

    assert.soon(() => isConfigCommitted(master));
    assert.commandWorked(master.adminCommand({replSetReconfig: config}));

    jsTestLog("replsets_priority1.js wait for 2 slaves");

    assert.soon(function() {
        rs.getPrimary();
        return rs._slaves.length == 2;
    }, "2 slaves");

    jsTestLog("replsets_priority1.js wait for new config version " + config.version);

    assert.soon(function() {
        var versions = [0, 0];
        rs._slaves[0].setSlaveOk();
        versions[0] = rs._slaves[0].getDB("local").system.replset.findOne().version;
        rs._slaves[1].setSlaveOk();
        versions[1] = rs._slaves[1].getDB("local").system.replset.findOne().version;
        return versions[0] == config.version && versions[1] == config.version;
    });

    jsTestLog("replsets_priority1.js awaitReplication");

    // the reconfiguration needs to be replicated! the hb sends it out
    // separately from the repl
    rs.awaitReplication();

    jsTestLog("reconfigured.  Checking statuses.");

    checkPrimaryIs(max);

    // Wait for election oplog entry to be replicated, to avoid rollbacks later on.
    rs.awaitReplication();

    jsTestLog("rs.stop");

    rs.stop(max._id);

    master = rs.getPrimary();

    jsTestLog("killed max primary.  Checking statuses.");

    jsTestLog("second is " + second.host + " with priority " + second.priority);
    checkPrimaryIs(second);

    // Wait for election oplog entry to be replicated, to avoid rollbacks later on.
    let liveSlaves = rs.nodes.filter(function(node) {
        return node.host !== max.host && node.host !== second.host;
    });
    rs.awaitReplication(null, null, liveSlaves);

    jsTestLog("restart max " + max._id);

    rs.restart(max._id);
    master = rs.getPrimary();

    jsTestLog("max restarted.  Checking statuses.");
    checkPrimaryIs(max);

    // Wait for election oplog entry to be replicated, to avoid rollbacks later on.
    rs.awaitReplication();
}

rs.stopSet();
})();
