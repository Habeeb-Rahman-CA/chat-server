# How to Chat with Friends from Terminal

This guide shows you how to host the production `chat_server` and have your friends connect from their own terminals.

---

## 1. Quick Start (Testing Locally)

### Terminal 1 (Host): Start the Server
```bash
cd /home/habrmnc/habrmnc/chat-server
./build/chat_server -p 8080
```

### Terminal 2 & 3 (You and a Friend on Same Machine): Connect
```bash
./build/chat_client -a 127.0.0.1 -p 8080
```

---

## 2. Option A: Over Home WiFi / Local Network (LAN)

If your friends are in the same apartment, house, dorm, or office on the same WiFi network:

1. **Find your LAN IP address**:
   ```bash
   ip -brief address show
   # Or: hostname -I
   ```
   Look for an IP like `192.168.1.50` or `10.0.0.15`.

2. **Start the server on `0.0.0.0` (all interfaces)**:
   ```bash
   ./build/chat_server -a 0.0.0.0 -p 8080
   ```

3. **Allow incoming TCP port 8080 in your firewall**:
   ```bash
   # On Ubuntu/Debian (ufw):
   sudo ufw allow 8080/tcp

   # On Fedora/RHEL (firewalld):
   sudo firewall-cmd --add-port=8080/tcp --permanent
   sudo firewall-cmd --reload
   ```

4. **Have your friends run the client**:
   ```bash
   ./build/chat_client -a 192.168.1.50 -p 8080
   ```

---

## 3. Option B: Over the Internet via Tailscale (Recommended!)

[Tailscale](https://tailscale.com/) is a zero-configuration, encrypted WireGuard mesh network. It gives every connected device a private `100.x.y.z` IP that works across firewalls, NATs, and cellular data without opening ports on your home router.

1. **Install Tailscale on your machine and your friends' machines**:
   ```bash
   curl -fsSL https://tailscale.com/install.sh | sh
   sudo tailscale up
   ```
2. **Find your Tailscale IP**:
   ```bash
   tailscale ip -4
   # Example: 100.85.120.45
   ```
3. **Start the chat server**:
   ```bash
   ./build/chat_server -a 0.0.0.0 -p 8080
   ```
4. **Friends join your Tailnet and connect**:
   ```bash
   ./build/chat_client -a 100.85.120.45 -p 8080
   ```

---

## 4. Option C: Instant Internet Exposure via Ngrok

If you want friends anywhere in the world to connect instantly without installing VPNs:

1. **Install and run ngrok TCP tunnel**:
   ```bash
   ngrok tcp 8080
   ```
2. **Ngrok will show an address like**:
   `tcp://0.tcp.ngrok.io:14567`
3. **Start your chat server locally**:
   ```bash
   ./build/chat_server -p 8080
   ```
4. **Friends connect directly**:
   ```bash
   ./build/chat_client -a 0.tcp.ngrok.io -p 14567
   # (Or using netcat: nc 0.tcp.ngrok.io 14567)
   ```

---

## 5. In-Chat Interactive Commands

Once inside `chat_client`:

| Command | Action | Example |
| :--- | :--- | :--- |
| `<message>` | Send a message to your current channel | `Hey everyone!` |
| `/join <#channel>` | Join or switch to a channel | `/join #gaming` or `/join #music` |
| `/rooms` | List active channels and user counts | `/rooms` |
| `/msg <user> <message>` | Send a private whisper | `/msg Alice check this out!` |
| `/users` | List all online friends and their channels | `/users` |
| `/nick <new_name>` | Change your display name | `/nick Shadow` |
| `/clear` | Clear terminal screen | `/clear` |
| `/quit` | Cleanly disconnect | `/quit` |
