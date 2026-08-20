const mqtt = require('mqtt');
const readline = require('readline');

const HOST = process.env.MQTT_HOST || 'mqtt://103.82.22.78:1883';
const DEVICE = process.env.DEVICE || 'cacao_2_rtk_base';
const CMD_TOPIC = `rtk/${DEVICE}/cmd`;
const LOG_TOPIC = `rtk/${DEVICE}/log`;

const client = mqtt.connect(HOST, {
    clientId: `rtk-cli-${Math.random().toString(16).slice(2, 10)}`,
    reconnectPeriod: 2000,
});

const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    prompt: '> ',
});

function ts() {
    return new Date().toISOString().slice(11, 19);
}

function line(tag, msg) {
    process.stdout.write(`\r[${ts()}] ${tag} ${msg}\n`);
    rl.prompt(true);
}

client.on('connect', () => {
    line('[MQTT]', `connected ${HOST}`);
    client.subscribe(LOG_TOPIC, err => {
        if (err) line('[ERR ]', `subscribe: ${err.message}`);
        else line('[SUB ]', LOG_TOPIC);
    });
    line('[HELP]', `type: FIXED | SURVEY | RESTART (or "exit")`);
    rl.prompt();
});

client.on('reconnect', () => line('[MQTT]', 'reconnecting...'));
client.on('close', () => line('[MQTT]', 'disconnected'));
client.on('error', err => line('[ERR ]', err.message));

client.on('message', (_topic, payload) => {
    line('[LOG ]', payload.toString());
});

rl.on('line', input => {
    const cmd = input.trim();
    if (!cmd) return rl.prompt();
    if (cmd === 'exit' || cmd === 'quit') {
        client.end();
        rl.close();
        return;
    }
    client.publish(CMD_TOPIC, cmd, { qos: 0 }, err => {
        if (err) line('[ERR ]', `publish: ${err.message}`);
        else line('[PUB ]', `${CMD_TOPIC} <- ${cmd}`);
    });
});

rl.on('close', () => {
    client.end();
    process.exit(0);
});

process.on('SIGINT', () => {
    client.end();
    rl.close();
});
