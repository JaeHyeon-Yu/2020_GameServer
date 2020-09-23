#include <iostream>
#include <WS2tcpip.h>
#include <MSWSock.h>
#include <vector>
#include <thread>
#include <mutex>
#include <unordered_set>
#include <atomic>
#include <chrono>
#include <queue>
#include <string>
#include <fstream>
#define UNICODE  
#include <sqlext.h>  

#include "protocol.h"
#pragma comment (lib, "WS2_32.lib")
#pragma comment (lib, "mswsock.lib")
#pragma comment (lib, "LUA/lua53.lib")

extern "C" {
#include "LUA/lua.h"
#include "LUA/lauxlib.h"
#include "LUA/lualib.h"
}

using namespace std;
using namespace chrono;
#define SERVER_ID -1
constexpr auto MAX_PACKET_SIZE = 255;
constexpr auto MAX_BUF_SIZE = 1024;
constexpr auto MAX_USER = 10'000;
constexpr auto MAX_VIEW = 5;
constexpr auto SECTOR_LEN = 20;
constexpr auto VIEW_RADIUS = 8;
constexpr auto MAX_NPC = 200'000;
constexpr auto ATK_POINT = 10;
enum ENUMOP { OP_RECV, OP_SEND, OP_ACCEPT, OP_NPC_MOVE, OP_RANDOM_MOVE, OP_PLAYER_MOVE, OP_ESCAPE_MOVE, OP_LOGIN, OP_LOGIN_FAIL, OP_UPDATE, OP_NPC_ATTACKED, OP_NEW_USER, OP_RECOVERY, OP_RESURRECTION };

enum C_STATUS { ST_FREE, ST_ALLOC, ST_ACTIVE, ST_SLEEP, ST_ESCAPE, ST_DEAD };
// 몬스터 AI Status
enum ATK_TYPE { PEACE, WAR };
enum MOVE_TYPE { HOLD, ROAM };
enum MONSTER_STATE { IDLE, BATTLE };

struct EXOVER {
	WSAOVERLAPPED	over;
	ENUMOP			op;
	char			io_buf[MAX_BUF_SIZE];
	union {
		WSABUF wsabuf;
		SOCKET c_socket;
		int p_id;
	};
};

struct CLIENT {
	mutex m_cl;
	SOCKET sock;
	int id;
	EXOVER recv_over;
	int prev_size;	// 받은 패킷의 사이즈
	char packet_buf[MAX_PACKET_SIZE];
	atomic<C_STATUS> status;
	atomic_bool isActive;
	// 데이터는 될 수 있도록 적은게 좋지만 확장성을 위해 short 사용
	short x, y;
	short defX, defY;
	char o_type;
	char name[MAX_ID_LEN + 1];	// id가 50바이트 꽉 차서 올 수 있으니까 혹시 몰라서 +1
	unsigned move_time;
	short sec_x, sec_y;
	short hp = 100;
	short level = 1;;
	short exp;
	short atk_point;
	unordered_set<int> viewList;	// 뷰리스트에 있는지가 중요하고 순서는 상관없기에 순서가 없으면 성능이 더 좋은 언오더드 사용

	short respawnCount = 0;

	// npc
	high_resolution_clock::time_point last_move_time;
	lua_State* L;
	mutex luaLock;
};

struct Event {
	int objID;
	ENUMOP eventID;
	high_resolution_clock::time_point wakeupTime;
	int targetID;

	constexpr bool operator<(const Event& e) const {
		return (wakeupTime > e.wakeupTime);
	}
};

struct DB_Event {
	ENUMOP eventID;
	int uid;
	char name[MAX_ID_LEN];
};

struct Position {
	unsigned int pos_x;
	unsigned int pos_y;
	unsigned int type;
};

void enter_game(int uid, char name[], int x, int y);
void ProcessPacket(int idx, char* buf);
void send_enter_packet(int uid, int oid);
void send_move_packet(int uidx, int mover);
void send_leave_packet(int uid, int oid);
void AddTimer(int oid, ENUMOP op, int duration);
void do_timer();
void Init_NPC();
void do_ai();
void random_move_npc(int id);
void ActivateNPC(int id);
void send_chat_packet(int uid, int chatter, wchar_t mess[]);
void DB_Thread();
void send_packet(int uid, void* p);

CLIENT g_clients[NPC_ID_START + NUM_NPC];
HANDLE g_iocp;
SOCKET listenSocket;

priority_queue<Event> timerQueue;
mutex timerLock;

queue<DB_Event> quaryQueue;
mutex quaryLock;

unordered_set<int> g_sector[WORLD_WIDTH / SECTOR_LEN][WORLD_HEIGHT / SECTOR_LEN];
mutex sectorLock;

bool map_data[WORLD_WIDTH][WORLD_HEIGHT];
Position read_monster[NUM_NPC];

void read_map() {
	ifstream fp;
	fp.open("map.txt");

	string number;
	int i = 0, j = 0;


	while (!fp.eof()) {
		fp >> number;
		if (number == "0xff")
		{
			if (j < WORLD_WIDTH)
			{
				map_data[i][j] = true;
				++j;
			}
			else
			{
				j = 0;
				++i;
				map_data[i][j] = true;
				++j;

			}
		}
		else if (number == " ")
		{

		}
		else
		{
			if (j < WORLD_WIDTH)
			{
				map_data[i][j] = false;
				++j;
			}
			else
			{
				j = 0;
				++i;
				map_data[i][j] = false;
				++j;

			}
		}
	}
}

void read_monster_data()
{
	ifstream fp("monster_position.txt");

	string number;
	int i = 0;


	while (!fp.eof()) {
		fp >> number;
		read_monster[i].pos_x = atoi(number.c_str());

		fp >> number;
		read_monster[i].pos_y = atoi(number.c_str());


		fp >> number;
		read_monster[i].type = atoi(number.c_str());
		
		++i;
		if (i == NUM_NPC)
			break;
	}

	fp.close();
}

bool In_AtkRange(int a, int b) {
	if (abs(g_clients[a].x - g_clients[b].x) > 1) return false;
	if (abs(g_clients[a].y - g_clients[b].y) > 1) return false;
	return true;
}

bool IsNear(int a, int b) {
	if (abs(g_clients[a].x - g_clients[b].x) > VIEW_RADIUS) return false;
	if (abs(g_clients[a].y - g_clients[b].y) > VIEW_RADIUS) return false;
	return true;
}

wstring ChangeUnicodeStr(const string& str) {
	int len;
	int slength = (int)str.length() + 1;
	len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), slength, 0, 0);
	wchar_t* buf = new wchar_t[len];
	MultiByteToWideChar(CP_ACP, 0, str.c_str(), slength, buf, len);
	std::wstring r(buf);
	delete[] buf;
	return r;
}

void LevelUp(int uid) {
	CLIENT& u = g_clients[uid];
	u.exp -= pow(2, u.level) * 100;	// 렙업 필요량만큼의 경험치 삭제후 레벨업 처리
	u.level++;
	u.hp = u.level * 100;
	wstring str = ChangeUnicodeStr(string(u.name) + "의 레벨이 올랐다 (" + to_string(u.level) + ")");
	send_chat_packet(uid, SERVER_ID, (wchar_t*)str.c_str());
}

void send_stat_change_packet(int uid) {
	sc_packet_stat_change p;
	p.type = S2C_STAT_CHANGE;
	p.size = sizeof(p);
	p.level = g_clients[uid].level;
	p.exp = g_clients[uid].exp;
	p.hp = g_clients[uid].hp;
	send_packet(uid, &p);
}

void NpcIsDead(int nid) {
	g_clients[nid].status = ST_DEAD;
	for (int i = 0; i < NPC_ID_START; ++i)
		if (g_clients[i].viewList.count(nid) != 0)
			send_leave_packet(i, nid);
	AddTimer(nid, OP_RESURRECTION, 30000);
}

void PlayerIsDead(int uid) {
	CLIENT& u = g_clients[uid];
	u.exp /= u.exp;
	u.hp = u.level * 100;
}

void Attack(int uid, int oid) {
	CLIENT& attacker = g_clients[uid];
	CLIENT& defender = g_clients[oid];
	if (defender.status == ST_DEAD) return;
	if (defender.viewList.empty())
		defender.viewList.insert(uid);
	
	defender.hp -= attacker.atk_point;
	wstring wstr = ChangeUnicodeStr((string)attacker.name + "가 " + (string)defender.name + "에게 " + to_string(attacker.atk_point) + "의 대미지 ");
	send_chat_packet(uid, SERVER_ID, (wchar_t*)wstr.c_str());

	if (defender.hp <= 0) {
		attacker.exp += defender.level * 5;
		wstr.clear();
		wstr = ChangeUnicodeStr((string)attacker.name + "가 " + to_string(defender.level * 5) + " 의 경험치 획득 ");
		send_chat_packet(uid, SERVER_ID, (wchar_t*)wstr.c_str());

		if (attacker.exp > pow(2, attacker.level) * 100) LevelUp(uid);
		send_stat_change_packet(uid);
		NpcIsDead(defender.id);
	}
}

void NPC_Attack(int oid, int uid) {
	CLIENT& attacker = g_clients[oid];
	CLIENT& defender = g_clients[uid];

	defender.hp -= attacker.atk_point;
	wstring wstr = ChangeUnicodeStr((string)attacker.name + "가 " + (string)defender.name + "에게 " + to_string(attacker.atk_point) + "의 대미지 ");
	send_chat_packet(uid, SERVER_ID, (wchar_t*)wstr.c_str());
	if (defender.hp <= 0) PlayerIsDead(uid);
	send_stat_change_packet(uid);
}

bool IsNearSector(int a, int b) {
	CLIENT& ca = g_clients[a];
	CLIENT& cb = g_clients[b];
	bool do_checkRight = (ca.sec_x * SECTOR_LEN + SECTOR_LEN / 2 < ca.x) && (cb.sec_x + 1 == ca.sec_x);
	bool do_checkLeft = ca.sec_x * SECTOR_LEN + SECTOR_LEN / 2 > ca.x && (cb.sec_x - 1 == ca.sec_x);
	bool do_checkDown = ca.sec_y * SECTOR_LEN + SECTOR_LEN / 2 < ca.y && (cb.sec_y + 1 == ca.sec_y);
	bool do_checkUp = ca.sec_y * SECTOR_LEN + SECTOR_LEN / 2 > ca.y && (cb.sec_y - 1 == ca.sec_y);

	// 섹터의 중앙좌표를 기준으로 주변 섹터를 검사
	if (do_checkRight) return true;
	if (do_checkLeft) return true;
	if (do_checkDown) return true;
	if (do_checkUp) return true;
	if (do_checkLeft && do_checkUp) return true;	// 좌상
	if (do_checkRight && do_checkUp) return true; // 우상
	if (do_checkLeft && do_checkDown) return true; // 좌하
	if (do_checkRight && do_checkDown) return true; // 우하
	return false;
}

int distance(int id1, int id2) {
	int x = pow((g_clients[id1].x - g_clients[id2].x), 2);
	int y = pow((g_clients[id1].y - g_clients[id2].y), 2);
	return sqrt(x + y);
}

bool In_ViewDistance(int id1, int id2) {
	if (id1 == id2) return false;
	return distance(id1, id2) <= MAX_VIEW;
}

void InitializeClients() {
	for (int i = 0; i < MAX_USER; ++i) {
		g_clients[i].id = i;
		g_clients[i].status = ST_FREE;
		g_clients[i].viewList.clear();
	}
}

void send_packet(int uid, void* p) {
	if (uid >= MAX_USER) return;
	char* buf = reinterpret_cast<char*>(p);
	CLIENT& u = g_clients[uid];
	// recv_over는 리시브용이라 쓰면안된다
	// 로컬변수로 선언하면 함수가 끝나면 재사용되어서 그대로 써도 안된다
	// 할당을 받아야함
	EXOVER* exover = new EXOVER;
	exover->op = OP_SEND;
	ZeroMemory(&exover->over, sizeof(exover->over));
	exover->wsabuf.buf = exover->io_buf;
	int sendBytes = (int)static_cast<unsigned char>(buf[0]);
	exover->wsabuf.len = sendBytes;
	memcpy(exover->io_buf, buf, sendBytes);
	WSASend(u.sock, &exover->wsabuf, 1, NULL, 0, &exover->over, NULL);
}

void send_leave_packet(int uid, int oid) {
	sc_packet_leave p;
	p.id = oid;
	p.size = sizeof(p);
	p.type = S2C_LEAVE;

	// 락이 있으면 락을 안한다 엔터런트 락이라고 따로 있음
	// 실제 상용서버에선 깔끔하게 안되고 모든 경우에 대한 예외처리를 해줘야해서 프로그램이 복잡해짐
	// 그걸 피하기 위해 여러 고려를 해줘야하는데 일단 참고 넘어가자

	g_clients[uid].m_cl.lock();
	g_clients[uid].viewList.erase(oid);
	g_clients[uid].m_cl.unlock();

	send_packet(uid, &p);
}

void disconnect(int uid) {
	send_leave_packet(uid, uid);
	g_clients[uid].m_cl.lock();
	g_clients[uid].status = ST_ALLOC;	// 이거도 아토믹
	// g_clients[uid].viewList.clear();
	closesocket(g_clients[uid].sock);
	for (int i = 0; i < NPC_ID_START; ++i) {
		CLIENT& cl = g_clients[i];
		if (cl.id == uid) continue;	// 이중락 조심
		//cl.m_cl.lock(); // enter와 마찬가지로 status를 아토믹으로 만들었으니까 락걸지말고 중간에 다른 애가 FREE로 하는 것만 없애자?
		// 이럴때 제대로 하려면 다른 스레드가 동시에 disconnect 호출했을때 한 스레드에서만 FREE로 해줘야함 그냥 해주면 안되고 아토믹으로 해줘야함

		//cl.m_cl.unlock();
	}
	g_clients[uid].status = ST_FREE;	// 이걸 아토믹으로 해야하는데 근데 그러면 프로그램 복잡해져서 없애고 버그 발생시 바꾸는걸로 상용에서는 절대 안됨
	g_clients[uid].m_cl.unlock();

	quaryLock.lock();
	DB_Event ev{ OP_UPDATE, uid };
	quaryQueue.push(ev);
	quaryLock.unlock();
}

void recv_packet_construct(int uid, int io_byte) {
	CLIENT& cu = g_clients[uid];
	EXOVER& r_o = cu.recv_over;

	int rest_byte = io_byte;
	char* p = r_o.io_buf;
	int packet_size = 0;
	if (cu.prev_size != 0) packet_size = cu.packet_buf[0];
	while (rest_byte > 0) {
		if (packet_size == 0) packet_size = (int)static_cast<unsigned char>(p[0]);
		if (packet_size <= rest_byte + cu.prev_size) {
			memcpy(cu.packet_buf + cu.prev_size, p, packet_size - cu.prev_size);
			p += packet_size - cu.prev_size;
			rest_byte -= packet_size - cu.prev_size;
			packet_size = 0;
			ProcessPacket(uid, cu.packet_buf);
			cu.prev_size = 0;

		}
		else {
			memcpy(cu.packet_buf + cu.prev_size, p, rest_byte);
			cu.prev_size += rest_byte;
			rest_byte = 0;
			p += rest_byte;
		}
	}
}

void worker_thread() {
	while (true) {
		DWORD io_byte;
		ULONG_PTR key;
		WSAOVERLAPPED* over;
		GetQueuedCompletionStatus(g_iocp, &io_byte, &key, &over, INFINITE);
		EXOVER* exover = reinterpret_cast<EXOVER*>(over);
		int user_id = static_cast<int>(key);
		CLIENT& cl = g_clients[user_id];


		switch (exover->op) {
		case OP_RECV:
			if (io_byte == 0) disconnect(user_id);
			else {
				recv_packet_construct(user_id, io_byte);

				ZeroMemory(&cl.recv_over.over, sizeof(cl.recv_over.over));
				DWORD flags = 0;
				WSARecv(cl.sock, &cl.recv_over.wsabuf, 1, NULL, &flags, &cl.recv_over.over, NULL);
			}
			break;
		case OP_SEND:
			if (io_byte == 0) disconnect(user_id);
			delete exover;
			break;
		case OP_ACCEPT: {
			int userId = -1;
			for (int i = 0; i < MAX_USER; ++i) {
				lock_guard<mutex> gl{ g_clients[i].m_cl };	// 블럭에서 빠져나가면 알아서 언락해줌 언락하고 다음 루프돌고 계속 반복 
				if (g_clients[i].status == ST_FREE) {
					g_clients[i].status = ST_ALLOC;
					userId = i;
					break;
				}
			}
			SOCKET clientSocket = exover->c_socket;
			if (user_id == -1) closesocket(clientSocket);
			else {
				CreateIoCompletionPort(reinterpret_cast<HANDLE>(clientSocket), g_iocp, userId, 0);
				CLIENT& nc = g_clients[userId];
				// nc.id = userId;
				nc.prev_size = 0;
				nc.recv_over.op = OP_RECV;
				// nc.recv_over.c_socket;

				ZeroMemory(&nc.recv_over.over, sizeof(nc.recv_over.over));
				nc.recv_over.wsabuf.buf = nc.recv_over.io_buf;
				nc.recv_over.wsabuf.len = MAX_BUF_SIZE;
				nc.sock = clientSocket;
				nc.x = rand() % WORLD_WIDTH;
				nc.y = rand() % WORLD_HEIGHT;
				nc.viewList.clear();
				DWORD flags = 0;
				WSARecv(clientSocket, &nc.recv_over.wsabuf, 1, NULL, &flags, &nc.recv_over.over, NULL);
			}

			clientSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
			exover->c_socket = clientSocket;
			ZeroMemory(&exover->over, sizeof(exover->over));
			AcceptEx(listenSocket, clientSocket, exover->io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &exover->over);
			// accept 받은 후에 계속해서 유지해줘야한다.
		}
						break;
		case OP_RANDOM_MOVE: {
			random_move_npc(user_id);
			bool keepAlive = false;
			for (int i = 0; i < NPC_ID_START; ++i)
				if (IsNear(user_id, i) && g_clients[i].status == ST_ACTIVE) {
					keepAlive = true;
					break;
				}
			if (keepAlive && g_clients[user_id].status == ST_ACTIVE)
				AddTimer(user_id, OP_RANDOM_MOVE, 1000);
			else g_clients[user_id].status = ST_SLEEP;
			delete exover;
		}
							 break;
		case OP_PLAYER_MOVE: {
			g_clients[user_id].luaLock.lock();
			lua_State* L = g_clients[user_id].L;
			lua_getglobal(L, "Event_Player_Move");
			lua_pushnumber(L, exover->p_id);
			int error = lua_pcall(L, 1, 1, 0);
			if (error) cout << lua_tostring(L, -1) << endl;
			bool isCollide = (bool)lua_toboolean(L, -1);
			lua_pop(L, 1);
			g_clients[user_id].luaLock.unlock();
			if (isCollide) {
				g_clients[user_id].status = ST_ESCAPE;
				AddTimer(user_id, OP_ESCAPE_MOVE, 1000);
			}
			delete exover;
		}
							 break;
		case OP_ESCAPE_MOVE: {
			g_clients[user_id].luaLock.lock();
			lua_State* L = g_clients[user_id].L;
			lua_getglobal(L, "Event_Escape_Move");
			int error = lua_pcall(L, 0, 3, 0);
			if (error) cout << lua_tostring(L, -1) << endl;
			int x = (int)lua_tointeger(L, -3);
			int y = (int)lua_tointeger(L, -2);
			int c = (int)lua_tointeger(L, -1);
			lua_pop(L, 3);
			g_clients[user_id].luaLock.unlock();

			g_clients[user_id].x = x;
			g_clients[user_id].y = y;

			for (int i = 0; i < NPC_ID_START; ++i) {
				if (g_clients[i].status != ST_ACTIVE) continue;
				if (IsNear(i, user_id)) {
					g_clients[i].m_cl.lock();
					if (g_clients[i].viewList.count(user_id) != 0) {
						g_clients[i].m_cl.unlock();
						send_move_packet(i, user_id);
					}
					else {
						g_clients[i].m_cl.unlock();
						send_enter_packet(i, user_id);
					}
				}
				else {
					g_clients[i].m_cl.lock();
					if (g_clients[i].viewList.count(user_id) != 0) {
						g_clients[i].m_cl.unlock();
						send_leave_packet(i, user_id);
					}
					else g_clients[i].m_cl.unlock();
				}
			}

			if (c != 0) AddTimer(user_id, OP_ESCAPE_MOVE, 1000);
			else {
				g_clients[user_id].status = ST_ACTIVE;
				AddTimer(user_id, OP_RANDOM_MOVE, 1000);
			}
			//	g_clients[user_id].m_cl.unlock();

			delete exover;
		}
							 break;
		case OP_LOGIN: {
			int x = exover->p_id / 1000;
			int y = exover->p_id % 1000;
			enter_game(user_id, exover->io_buf, x, y);
			delete exover;
		} break;
		case OP_LOGIN_FAIL:
			disconnect(user_id);
			break;
		case OP_RECOVERY:
			g_clients[user_id].hp += g_clients[user_id].level * 10;
			if (g_clients[user_id].hp > g_clients[user_id].level * 100)
				g_clients[user_id].hp = g_clients[user_id].level * 100;
			send_stat_change_packet(user_id);
			AddTimer(user_id, OP_RECOVERY, 5000);
			
			break;
		case OP_RESURRECTION:
			g_clients[user_id].hp = 20;
			g_clients[user_id].x = g_clients[user_id].x;
			g_clients[user_id].y = g_clients[user_id].y;
			g_clients[user_id].status = ST_SLEEP;
			wstring wstr = ChangeUnicodeStr((string)g_clients[user_id].name + " 부활 ");
			for (auto& cl : g_clients) {
				if (cl.id >= NPC_ID_START) break;
				if (cl.status == ST_ACTIVE)
					send_chat_packet(cl.id, SERVER_ID, (wchar_t*)wstr.c_str());
			}
			break;
		}
	}
}

bool Is_Player(int id) {
	return id < NPC_ID_START;
}

int main() {
	read_map();
	read_monster_data();
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);

	cout << "NPC Initialize Start" << endl;
	Init_NPC();
	cout << "NPC Initialize Finished" << endl;

	listenSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	SOCKADDR_IN serverAddr;
	// memset(&serverAddr, 0, sizeof(serverAddr));
	ZeroMemory(&serverAddr, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(SERVER_PORT);
	serverAddr.sin_addr.S_un.S_addr = htonl(INADDR_ANY);
	::bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
	// 그냥 쓰면 C++11의 bind 함수와 연결될 수 있다 -> 그래서 콜론 붙힘

	listen(listenSocket, SOMAXCONN);


	g_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, 0);

	InitializeClients();

	CreateIoCompletionPort(reinterpret_cast<HANDLE>(listenSocket), g_iocp, 999, 0);
	SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);;
	EXOVER accept_over;
	ZeroMemory(&accept_over.over, sizeof(accept_over.over));
	accept_over.op = OP_ACCEPT;
	accept_over.c_socket = clientSocket;
	AcceptEx(listenSocket, clientSocket, accept_over.io_buf, NULL, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &accept_over.over);
	// 여기선 반환하는게 아니라 미리 소켓을 만들어서 파라미터로 넣어줘야함
	// accpet 버퍼가 필요한데 버퍼 주소를 넣기위해 필요하다?
	// 접속할때 클라주소도 필요한데 어떤 네트워크 카드로 넘어갔는가도 필요해서 수신, 송신 주소 모두 필요하ㅣㅁ
	// sizeof~~~ 주소의 크기 +16은 좀 여유롭게 넣어주는것
	// thread timer_thread{ TimerThread };
	vector<thread> worker_threads;
	for (int i = 0; i < 10; ++i)
		worker_threads.emplace_back(worker_thread);
	thread timer_thread{ do_timer };
	thread db_thread{ DB_Thread };
	for (auto& th : worker_threads) th.join();
	timer_thread.join();
	db_thread.join();
}

void send_login_ok_packet(int uidx) {
	sc_packet_login_ok p;
	p.exp = g_clients[uidx].exp;
	p.hp = g_clients[uidx].hp;
	p.id = uidx;
	p.level = g_clients[uidx].level;
	p.size = sizeof(p);
	p.type = S2C_LOGIN_OK;
	p.x = g_clients[uidx].x;
	p.y = g_clients[uidx].y;

	send_packet(uidx, &p);
}

void send_move_packet(int uidx, int mover) {
	sc_packet_move p;
	p.id = mover;
	p.size = sizeof(p);
	p.type = S2C_MOVE;
	p.x = g_clients[mover].x;
	p.y = g_clients[mover].y;
	p.move_time = g_clients[mover].move_time;
	send_packet(uidx, &p);
}

void send_chat_packet(int uid, int chatter, wchar_t mess[]) {
	sc_packet_chat p;
	p.id = chatter;
	p.size = sizeof(p);
	p.type = S2C_CHAT;
	// strcpy_s(p.mess, mess);
	wcscpy_s(p.mess, mess);
	send_packet(uid, &p);
}

void do_move(int uidx, int direction) {
	CLIENT& u = g_clients[uidx];
	int x = u.x;
	int y = u.y;
	switch (direction) {
	case D_UP:
		if (y > 0 && map_data[y-1][x]) y--;
		break;
	case D_DOWN:
		if (y < WORLD_HEIGHT - 1 && map_data[y+1][x]) y++;
		break;
	case D_LEFT:
		if (x > 0 && map_data[y][x-1]) x--;
		break;
	case D_RIGHT:
		if (x < WORLD_WIDTH - 1 && map_data[y][x+1]) x++;
		break;
	default:
		cout << "Unknown Direction from Client move packet!\n";
		DebugBreak();
		exit(-1);
	}
	u.x = x;
	u.y = y;
	u.sec_x = x / SECTOR_LEN;
	u.sec_y = y / SECTOR_LEN;

	// 이동 후 뷰리스트를 새로 만들어야함
	g_clients[uidx].m_cl.lock();
	unordered_set<int> oldVl = g_clients[uidx].viewList;
	// 그대로 쓰면 락걸고 액세스해야하는데 그러면 좀 그러니까 복사해서 사용
	// 대신 복사한 다음에 다른 스레드에서 뷰리스트를 수정하는 것은 감안해야함
	// 뷰리스트로 관리해서 살짝 어긋나더라도 돌아온다
	g_clients[uidx].m_cl.unlock();
	unordered_set<int> newVl;
	for (auto& cl : g_clients) {
		if (!IsNear(cl.id, uidx)) continue;
		if (cl.status == ST_SLEEP) {
			ActivateNPC(cl.id);
			
		}
		if (cl.status != ST_ACTIVE) continue;
		if (cl.id == u.id) continue;
		if (!Is_Player(cl.id)) {
			EXOVER* exover = new EXOVER;
			exover->op = OP_PLAYER_MOVE;
			exover->p_id = uidx;
			PostQueuedCompletionStatus(g_iocp, 1, cl.id, &exover->over);
		}

		/// // 2칸 이상 떨어져있으면 시야거리가 너무 멀어서 볼 필요도 없으니 패스
		/// if (abs(cl.sec_x - u.sec_x) > 1) continue;
		/// if (abs(cl.sec_y - u.sec_y) > 1) continue;
		/// if (cl.sec_x == u.sec_x && cl.sec_y == u.sec_y)
		/// 	newVl.insert(cl.id);
		/// else if (IsNearSector(u.id, cl.id))
			newVl.insert(cl.id);
	}

	send_move_packet(u.id, u.id);	// 뷰리스트엔 자신을 넣지 않기에 자신의 것은 따로 보낸다.

	// 시야에 새로 들어온 플레이어
	for (auto& np : newVl) {
		if (oldVl.count(np) == 0) {			// 오브젝트가 새로 시야에 들어왔을때
			send_enter_packet(u.id, np);
			if (!Is_Player(np)) continue;
			g_clients[np].m_cl.lock();
			if (g_clients[np].viewList.count(u.id) == 0) {
				g_clients[np].m_cl.unlock();
				send_enter_packet(np, u.id);
			}
			else {
				g_clients[np].m_cl.unlock();
				send_move_packet(np, u.id);
			}
		}
		else {								// 계속 시야에 존재하고 있을 때
			if (!Is_Player(np)) continue;
			g_clients[np].m_cl.lock();
			if (g_clients[np].viewList.count(u.id) != 0) {
				g_clients[np].m_cl.unlock();
				send_move_packet(np, u.id);
			}
			else {
				g_clients[np].m_cl.unlock();
				send_enter_packet(np, u.id);
			}
		}
	}

	for (auto& op : oldVl) {		// d오브젝트가 시야에서 벗어났을 대
		if (newVl.count(op) == 0) {
			send_leave_packet(u.id, op);
			if (!Is_Player(op)) continue;
			g_clients[op].m_cl.lock();
			if (g_clients[op].viewList.count(u.id) != 0) {
				g_clients[op].m_cl.unlock();
				send_leave_packet(op, u.id);
			}
			else {
				g_clients[op].m_cl.unlock();
			}
		}
	}

}

void send_enter_packet(int uid, int oid) {
	sc_packet_enter p;
	p.id = oid;
	p.size = sizeof(p);
	p.type = S2C_ENTER;
	p.x = g_clients[oid].x;
	p.y = g_clients[oid].y;
	strcpy_s(p.name, g_clients[oid].name);
	p.o_type = g_clients[oid].o_type;
	g_clients[uid].m_cl.lock();
	g_clients[uid].viewList.insert(oid);
	g_clients[uid].m_cl.unlock();

	send_packet(uid, &p);
}

void enter_game(int uid, char name[], int x, int y) {
	CLIENT& u = g_clients[uid];
	u.m_cl.lock();
	u.x = x;
	u.y = y;
	if (u.id < NPC_ID_START)
		u.o_type = O_HUMAN;
	strcpy_s(u.name, name);
	u.name[MAX_ID_LEN] = NULL;
	send_login_ok_packet(uid);	// 빼내면 좋은데 빼내기에는 락 안에서 네임을 수정해야하고 뭐하고 골치아픔 -> 이거때문에 충돌 많지는 않을거같으니 냅두기
	g_clients[uid].status = ST_ACTIVE;
	u.m_cl.unlock();

	u.sec_x = x / SECTOR_LEN;
	u.sec_y = y / SECTOR_LEN;
	AddTimer(uid, OP_RECOVERY, 5000);

	// g_sector[u.sec_x][u.sec_y].insert(uid);	// s

	for (auto& cl : g_clients) {
		int i = cl.id;
		if (uid == i) continue;
		if (IsNear(u.id, i)) {
			// g_clients[i].m_cl.lock();
			if (cl.status == ST_SLEEP)
				ActivateNPC(cl.id);
			if (g_clients[i].status == ST_ACTIVE) {
				send_enter_packet(uid, i);
				if (Is_Player(i))
					send_enter_packet(i, uid);	// 위에서 락거는데 이 안에서 락거니까 충돌남 정확히는 락을 걸고 보내는 것과 안걸고 보내는 것 두 가지로 나눠야함

				// 원래는 status가 데이터레이스라 락을 걸어야하는데 타협하고 락을 걸지 않는 것으로 타협
				// 만약 보내려는데 다른 스레드가 ACTIVE가 아니게 바꾸는 경우는... ACTIVE를 확인하고 동작하는 스레드가 종료한 후에 ACTIVE를 FREE로 바꿔야한다?
				// 다른 스레드가 건드리나 안건드리나 봐야하는데 이걸 구현해야하고 안그럼 오동작을 일으킴
				// 근데 그건 고급테크닉이 들어가기 때문에 일단 무시하고 문제가 생기면 수정하는 것으로 한다. 
				// 실습서버와 상용서버는 성능, 안정성 면에서 차이가 있다. 상용서버에서는 이를 절대로 냅두면안됨
			}
			// g_clients[i].m_cl.unlock();
		}
	}
}

void ProcessPacket(int idx, char* buf) {
	switch (buf[1]) {
	case C2S_LOGIN: {
		cs_packet_login* packet = reinterpret_cast<cs_packet_login*>(buf);
		// login
		DB_Event ev{ OP_LOGIN, idx };
		strcpy_s(ev.name, packet->name);
		quaryLock.lock();
		quaryQueue.push(ev);
		quaryLock.unlock();
	}
					break;
	case C2S_MOVE: {
		cs_packet_move* packet = reinterpret_cast<cs_packet_move*>(buf);
		g_clients[idx].move_time = packet->move_time;
		do_move(idx, packet->direction);
	}
				   break;
	case C2S_ATTACK: {
		CLIENT& u = g_clients[idx];
		auto viewList = u.viewList;
		for (auto& o : viewList) {
			auto& cl = g_clients[o];
			if (u.x == cl.x && u.y == cl.y + 1) Attack(u.id, o);
			if (u.x == cl.x && u.y == cl.y - 1) Attack(u.id, o);
			if (u.x == cl.x+1 && u.y == cl.y) Attack(u.id, o);
			if (u.x == cl.x-1 && u.y == cl.y) Attack(u.id, o);
		}
	}break;
	case C2S_LOGOUT:
		disconnect(idx);
		break;
	case C2S_CHAT: {
		cout << "chat : ";
		cs_packet_chat* packet = reinterpret_cast<cs_packet_chat*>(buf);
		wcout << packet->message << endl;
		for (auto& cl : g_clients) {
			if (cl.id >= NPC_ID_START) break;
			if (cl.status != ST_ACTIVE) continue;
			send_chat_packet(cl.id, idx, packet->message);
		}
		// send_chat_packet(idx, idx, packet->message);
	}break;

	default:
		cout << "Unknwon Packet Type Error!\n" << endl;
		DebugBreak(); // 여기서 멈추고 브레이크 포인트 걸린거처럼 상태를 표시한다
		exit(-1);
	}
}
/*
클라이언트의 접속 종료
- GetQueuedCompletionStatus의 io_bytes가 0인 경우
-> 0을 받거나 보냈다, -> 상대가 close해서 0을 주고받은거임
*/

int API_SendMessage(lua_State* L) {
	int my_id = (int)lua_tointeger(L, -3);
	int user_id = (int)lua_tointeger(L, -2);
	char* mess = (char*)lua_tostring(L, -1);
	//wchar_t* wmess = L"";
	string str(mess);
	wstring wstr = L"";
	wstr.assign(str.begin(), str.end());
	send_chat_packet(user_id, my_id, (wchar_t*)wstr.c_str());
	lua_pop(L, 3);
	return 0;
}

int API_Get_x(lua_State* L) {
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x = g_clients[obj_id].x;
	lua_pushnumber(L, x);
	return 1;
}

int API_Get_y(lua_State* L) {
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int y = g_clients[obj_id].y;
	lua_pushnumber(L, y);
	return 1;
}

int API_Set_Pos(lua_State* L) {
	int obj_id = (int)lua_tointeger(L, -1);
	lua_pop(L, 2);
	int x = g_clients[obj_id].x;
	lua_pushnumber(L, x);
	return 1;

}

// 실습
void Init_NPC() {
	for (int i = NPC_ID_START; i < NPC_ID_START + NUM_NPC; ++i) {
		g_clients[i].sock = 0;
		g_clients[i].id = i;
		sprintf_s(g_clients[i].name, "NPC%d", i);
		g_clients[i].status = ST_SLEEP;
		g_clients[i].x = read_monster[i-NPC_ID_START].pos_x;
		g_clients[i].y = read_monster[i-NPC_ID_START].pos_y;
		g_clients[i].defX = g_clients[i].x;
		g_clients[i].defY = g_clients[i].y;
		g_clients[i].o_type = read_monster[i-NPC_ID_START].type;
		g_clients[i].sec_x = g_clients[i].x / SECTOR_LEN;
		g_clients[i].sec_y = g_clients[i].y / SECTOR_LEN;
		
		
		g_clients[i].level = 1;
		g_clients[i].hp = 20;
		g_clients[i].atk_point = 5;

		lua_State* L = g_clients[i].L = luaL_newstate();
		luaL_openlibs(L);
		luaL_loadfile(L, "NPC.LUA");
		lua_pcall(L, 0, 0, 0);
		lua_getglobal(L, "SetUID");
		lua_pushnumber(L, i);
		lua_pcall(L, 1, 0, 0);
		lua_pop(L, 1);

		lua_register(L, "API_SendMessage", API_SendMessage);
		lua_register(L, "API_Get_x", API_Get_x);
		lua_register(L, "API_Get_y", API_Get_y);

	}
}

void random_move_npc(int id) {
	if (g_clients[id].status != ST_ACTIVE) return;
	for (auto& cl : g_clients) {
		if (cl.id >= NPC_ID_START) break;
		if (cl.status != ST_ACTIVE) continue;
		if (In_AtkRange(id, cl.id)) {
			NPC_Attack(id, cl.id);
			return;
		}
	}
	int x = g_clients[id].x;
	int y = g_clients[id].y;
	switch (rand() % 4) {	//32
	case D_UP:
		if (y > 0 && map_data[y - 1][x]) y--;
		break;
	case D_DOWN:
		if (y < WORLD_HEIGHT - 1 && map_data[y + 1][x]) y++;
		break;
	case D_LEFT:
		if (x > 0 && map_data[y][x-1]) x--;
		break;
	case D_RIGHT:
		if (x < WORLD_WIDTH - 1 && map_data[y][x + 1]) x++;
		break;
	}

	g_clients[id].x = x;
	g_clients[id].y = y;

	g_clients[id].sec_x = x / SECTOR_LEN;
	g_clients[id].sec_y = y / SECTOR_LEN;

	for (int i = 0; i < NPC_ID_START; ++i) {
		if (g_clients[i].status != ST_ACTIVE) continue;
		// if (!IsNearSector(id, i)) continue;
		if (IsNear(i, id)) {
			g_clients[i].m_cl.lock();
			if (g_clients[i].viewList.count(id) != 0) {
				g_clients[i].m_cl.unlock();
				send_move_packet(i, id);
			}
			else {
				g_clients[i].m_cl.unlock();
				send_enter_packet(i, id);
			}
		}
		else {
			g_clients[i].m_cl.lock();
			if (g_clients[i].viewList.count(id) != 0) {
				g_clients[i].m_cl.unlock();
				send_leave_packet(i, id);
			}
			else g_clients[i].m_cl.unlock();
		}
	}
}

void AddTimer(int oid, ENUMOP op, int duration) {
	Event ev{ oid, op, high_resolution_clock::now() + milliseconds(duration), 0 };
	timerLock.lock();
	timerQueue.push(ev);
	timerLock.unlock();
}

void do_timer() {
	while (true) {
		this_thread::sleep_for(1ms);
		while (true) {
			timerLock.lock();
			if (timerQueue.empty()) {
				timerLock.unlock();
				break;
			}
			if (timerQueue.top().wakeupTime > high_resolution_clock::now()) {
				timerLock.unlock();
				break;
			}
			Event ev = timerQueue.top();
			timerQueue.pop();
			timerLock.unlock();
			if (g_clients[ev.objID].status != ST_ACTIVE && g_clients[ev.objID].status != ST_ESCAPE && ev.eventID != OP_RESURRECTION) break;
			switch (ev.eventID) {
			case OP_RANDOM_MOVE:
			case OP_ESCAPE_MOVE:
			case OP_RECOVERY:
			case OP_RESURRECTION:
				EXOVER* exover = new EXOVER;
				exover->op = ev.eventID;
				PostQueuedCompletionStatus(g_iocp, 1, ev.objID, &exover->over);	// 오버랩드 구조체 초기화안해도 send.recv아니면 오동작 안함
				break;
			}
		}
	}
}

void ActivateNPC(int id) {
	C_STATUS oldState = ST_SLEEP;
	if (atomic_compare_exchange_strong(&g_clients[id].status, &oldState, ST_ACTIVE))
		AddTimer(id, OP_RANDOM_MOVE, 1000);
}

void DB_Thread() {
	SQLHENV henv;
	SQLHDBC hdbc;
	SQLHSTMT hstmt = 0;
	SQLRETURN retcode;
	SQLCHAR isLogin;
	SQLINTEGER user_x{}, user_y{}, user_level{}, user_exp{}, user_atk{};
	SQLWCHAR userName[MAX_ID_LEN];

	SQLLEN cbx = 0, cby = 0, cbid = 0;
	SQLLEN cblevel = 0, cbexp = 0, cbatk = 0;
	setlocale(LC_ALL, "korean");
	while (true) {
		this_thread::sleep_for(1ms);
		while (true) {
			quaryLock.lock();
			if (quaryQueue.empty()) {
				quaryLock.unlock();
				break;
			}
			auto ev = quaryQueue.front();
			quaryQueue.pop();
			quaryLock.unlock();
			// Allocate environment handle  
			retcode = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &henv);

			// Set the ODBC version environment attribute  
			if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
				retcode = SQLSetEnvAttr(henv, SQL_ATTR_ODBC_VERSION, (SQLPOINTER*)SQL_OV_ODBC3, 0);

				// Allocate connection handle  
				if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
					retcode = SQLAllocHandle(SQL_HANDLE_DBC, henv, &hdbc);

					// Set login timeout to 5 seconds  
					if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
						SQLSetConnectAttr(hdbc, SQL_LOGIN_TIMEOUT, (SQLPOINTER)5, 0);

						// Connect to data source  
						retcode = SQLConnect(hdbc, (SQLWCHAR*)L"game_db_odbc", SQL_NTS, (SQLWCHAR*)NULL, 0, NULL, 0);

						// Allocate statement handle  
						if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
							// std::cout << "ODBC connect OK!\n";
							retcode = SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt);

							switch (ev.eventID) {
							case OP_LOGIN: {
								string sql = "EXEC Login " + (string)ev.name;
								wstring strUni = L"";
								strUni.assign(sql.begin(), sql.end());
								retcode = SQLExecDirect(hstmt, (SQLWCHAR*)strUni.c_str(), SQL_NTS);
								if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {

									retcode = SQLBindCol(hstmt, 1, SQL_C_LONG, &user_x, 100, &cbx);
									retcode = SQLBindCol(hstmt, 2, SQL_C_LONG, &user_y, 100, &cby);
									retcode = SQLBindCol(hstmt, 3, SQL_C_LONG, &user_level, 100, &cblevel);
									retcode = SQLBindCol(hstmt, 4, SQL_C_LONG, &user_exp, 100, &cbexp);
									retcode = SQLBindCol(hstmt, 5, SQL_C_LONG, &user_atk, 100, &cbatk);

									retcode = SQLFetch(hstmt);
									bool isUser = false;
									if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
										for (auto& cl : g_clients) {
											if (cl.id >= NPC_ID_START) break;
											if (cl.id == ev.uid) continue;
											if (!strcmp(cl.name, ev.name)) {
												EXOVER* exover = new EXOVER;
												exover->op = OP_LOGIN_FAIL;
												PostQueuedCompletionStatus(g_iocp, 1, ev.uid, &exover->over);
												break;
											}

										}

										EXOVER* exover = new EXOVER;
										exover->op = OP_LOGIN;
										exover->p_id = user_x * 1000 + user_y;
										g_clients[ev.uid].level = user_level;
										g_clients[ev.uid].exp = user_exp;
										g_clients[ev.uid].atk_point = user_atk;
										strcpy_s(exover->io_buf, ev.name);
										PostQueuedCompletionStatus(g_iocp, 1, ev.uid, &exover->over);
									}
									else {
										DB_Event new_ev;
										new_ev.uid = ev.uid;
										new_ev.eventID = OP_NEW_USER;
										strcpy_s(new_ev.name, ev.name);
										quaryLock.lock();
										quaryQueue.push(new_ev);
										quaryLock.unlock();
									}
								}


							}break;
							case OP_UPDATE: {
								string sql = "EXEC Update_UserData " + (string)g_clients[ev.uid].name + ", " +
									to_string(g_clients[ev.uid].x) + ", " + to_string(g_clients[ev.uid].y) + ", " + to_string(g_clients[ev.uid].level) + ", " + to_string(g_clients[ev.uid].exp) + ", " + to_string(g_clients[ev.uid].hp);
								wstring strUni = L"";
								strUni.assign(sql.begin(), sql.end());
								retcode = SQLExecDirect(hstmt, (SQLWCHAR*)strUni.c_str(), SQL_NTS);
								if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
									SQLFetch(hstmt);
								}
								
							}break;
							case OP_NEW_USER: {
								string sql = "EXEC New_User " + string(ev.name) + ", ";
								char n[10] = "";
								int x = rand() % WORLD_WIDTH;
								itoa(x, n, 10);
								sql += string(n);
								int y = rand() % WORLD_HEIGHT;
								itoa(y, n, 10);
								sql += ", ";
								sql += string(n);

								wstring wsql = L"";
								wsql.assign(sql.begin(), sql.end());
								wcout << wsql;
								retcode = SQLExecDirect(hstmt, (SQLWCHAR*)wsql.c_str(), SQL_NTS);

								if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
									EXOVER* exover = new EXOVER;
									exover->op = OP_LOGIN;
									exover->p_id = x * 1000 + y;
									g_clients[ev.uid].level = 1;
									g_clients[ev.uid].exp = 0;
									strcpy_s(exover->io_buf, ev.name);
									PostQueuedCompletionStatus(g_iocp, 1, ev.uid, &exover->over);
								}

							}break;
							default:
								break;
							}


							// Process data  
							if (retcode == SQL_SUCCESS || retcode == SQL_SUCCESS_WITH_INFO) {
								SQLCancel(hstmt);
								SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
							}

							SQLDisconnect(hdbc);
						}

						SQLFreeHandle(SQL_HANDLE_DBC, hdbc);
					}
				}
				SQLFreeHandle(SQL_HANDLE_ENV, henv);
			}
		}
	}
}