#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <windows.h>
#include <iostream>
#include <unordered_map>
#include <chrono>
#include <vector>

using namespace std;
using namespace chrono;

#include "..\..\IOCPGameServer\IOCPGameServer\protocol.h"

sf::TcpSocket g_socket;

constexpr auto SCREEN_WIDTH = 20;
constexpr auto SCREEN_HEIGHT = 20;

constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = TILE_WIDTH * SCREEN_WIDTH / 2 + 10;   // size of window
constexpr auto WINDOW_HEIGHT = TILE_WIDTH * SCREEN_WIDTH / 2 + 10;
constexpr auto BUF_SIZE = 200;
constexpr auto MAX_USER = NPC_ID_START;

int g_left_x;
int g_top_y;
int g_myid;

sf::RenderWindow* g_window;
sf::Font g_font;
/******************************************************************/
#include <fstream>
#include <string>
bool map_data[WORLD_WIDTH][WORLD_HEIGHT];

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

				printf("%d\n", i);
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

/******************************************************************/

class CHATTING {
private:
	vector<sf::Text> chat_log;
public:
	void add_chat(char chat[]) {
		sf::Text text;
		text.setFont(g_font);
		text.setString(chat);
		text.scale({ 1.5,1.5 });
		chat_log.emplace(chat_log.begin(), text);

		if (chat_log.size() > 10)
			chat_log.erase(chat_log.end() - 1);

	}
	void add_chat_unicode(wchar_t chat[]) {
		sf::Text text;
		text.setFont(g_font);
		text.setString(chat);
		text.scale({ 1.5,1.5 });
		chat_log.emplace(chat_log.begin(), text);

		if (chat_log.size() > 10)
			chat_log.erase(chat_log.end() - 1);
	}
	void draw_chat() {
		for (int i = 0; i < chat_log.size(); ++i)
		{
			chat_log[i].setPosition(0, WINDOW_HEIGHT * 2 - 100 - i * 30);
			g_window->draw(chat_log[i]);
		}

	}
};

class OBJECT {
private:
	bool m_showing;
	sf::Sprite m_sprite;

	char m_mess[MAX_STR_LEN];
	high_resolution_clock::time_point m_time_out;
	sf::Text m_text;
	sf::Text m_name;

public:
	int m_x, m_y;
	char name[MAX_ID_LEN];
	int m_level;
	int m_hp;
	int m_exp;
	int type;
	OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
		m_showing = false;
		m_sprite.setTexture(t);
		m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
		m_time_out = high_resolution_clock::now();
	}
	OBJECT() {
		m_showing = false;
		m_time_out = high_resolution_clock::now();
	}
	void show()
	{
		m_showing = true;
	}
	void hide()
	{
		m_showing = false;
	}

	void a_move(int x, int y) {
		m_sprite.setPosition((float)x, (float)y);
	}

	void a_draw() {
		g_window->draw(m_sprite);
	}

	void move(int x, int y) {
		m_x = x;
		m_y = y;
	}
	void draw() {
		if (false == m_showing) return;
		float rx = (m_x - g_left_x) * 65.0f + 8;
		float ry = (m_y - g_top_y) * 65.0f + 8;
		m_sprite.setPosition(rx, ry);
		g_window->draw(m_sprite);
		m_name.setPosition(rx - 10, ry - 10);
		g_window->draw(m_name);
		if (high_resolution_clock::now() < m_time_out) {
			m_text.setPosition(rx - 10, ry + 15);
			g_window->draw(m_text);
		}
	}
	void set_name(char str[]) {
		m_name.setFont(g_font);
		m_name.setString(str);
		m_name.setFillColor(sf::Color(255, 255, 0));
		m_name.setStyle(sf::Text::Bold);
	}
	void add_chat(char chat[]) {
		m_text.setFont(g_font);
		m_text.setString(chat);
		m_time_out = high_resolution_clock::now() + 1s;
	}
	void add_chat_unicode(wchar_t chat[]) {
		m_text.setFont(g_font);
		m_text.setString(chat);
		m_time_out = high_resolution_clock::now() + 1s;
	}
	void add_chat_unicode(char name[], wchar_t chat[]) {
		m_text.setFont(g_font);
		wstring wchat = ChangeUnicodeStr(string(name)) + chat;
		m_text.setString(wchat);
		m_time_out = high_resolution_clock::now() + 1s;
	}
};

OBJECT avatar;
unordered_map <int, OBJECT> npcs;

OBJECT white_tile;
OBJECT black_tile;

sf::Texture* board;
sf::Texture* pieces;

CHATTING chatting;
wstring currentChat;

void client_initialize()
{
	board = new sf::Texture;
	pieces = new sf::Texture;
	if (false == g_font.loadFromFile("NanumGothic.ttf")) {
		cout << "Font Loading Error!\n";
		while (true);
	}
	auto tile1 = new sf::Texture;
	auto tile2 = new sf::Texture;
	
	pieces->loadFromFile("hero.png");
	tile1->loadFromFile("tile1.png");
	white_tile = OBJECT{ *tile1, 0, 0, TILE_WIDTH, TILE_WIDTH };
	tile2->loadFromFile("tile2.png");
	black_tile = OBJECT{ *tile2, 0, 0, TILE_WIDTH, TILE_WIDTH };
	avatar = OBJECT{ *pieces, 0, 0, 65, 65 };
	avatar.move(4, 4);
}

void client_finish()
{
	delete board;
	delete pieces;
}

void ProcessPacket(char* ptr)
{
	static bool first_time = true;
	switch (ptr[1])
	{
	case S2C_LOGIN_OK:
	{
		sc_packet_login_ok* my_packet = reinterpret_cast<sc_packet_login_ok*>(ptr);
		g_myid = my_packet->id;
		avatar.move(my_packet->x, my_packet->y);
		avatar.m_exp = my_packet->exp;
		avatar.m_hp = my_packet->hp;
		avatar.m_level = my_packet->level;
		g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
		g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
		avatar.show();
	}
	break;

	case S2C_ENTER:
	{
		sc_packet_enter* my_packet = reinterpret_cast<sc_packet_enter*>(ptr);
		int id = my_packet->id;
		if (id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			pieces->loadFromFile("hero.png");
			avatar = OBJECT{ *pieces, 0, 0, 65, 65 };
			
			//g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			//g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
			avatar.show();
			avatar.type = my_packet->o_type;
		}
		else {
			// npcs[id].type 
			// npcs[id].
			// pieces->loadFromFile("monster.png");
			// avatar = OBJECT{ *pieces, 0, 0, 65, 65 };

			if (id < NPC_ID_START) {
				pieces->loadFromFile("hero.png");

				npcs[id] = OBJECT{ *pieces, 0, 0, 65, 65 };
			}
			else {
				auto ns = new sf::Texture;
				ns->loadFromFile("monster.png");
				// pieces->loadFromFile("monster.png");
				npcs[id] = OBJECT{ *ns, 0, (my_packet->o_type)*65, 65, 65 };
			}
			strcpy_s(npcs[id].name, my_packet->name);
			npcs[id].set_name(my_packet->name);
			npcs[id].move(my_packet->x, my_packet->y);
			npcs[id].show();
		}
	}
	break;
	case S2C_MOVE:
	{
		sc_packet_move* my_packet = reinterpret_cast<sc_packet_move*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.move(my_packet->x, my_packet->y);
			g_left_x = my_packet->x - (SCREEN_WIDTH / 2);
			g_top_y = my_packet->y - (SCREEN_HEIGHT / 2);
		}
		else {
			if (0 != npcs.count(other_id))
				npcs[other_id].move(my_packet->x, my_packet->y);
		}
	}
	break;

	case S2C_LEAVE:
	{
		sc_packet_leave* my_packet = reinterpret_cast<sc_packet_leave*>(ptr);
		int other_id = my_packet->id;
		if (other_id == g_myid) {
			avatar.hide();
		}
		else {
			if (0 != npcs.count(other_id))
				npcs[other_id].hide();
		}
	}
	break;
	case S2C_CHAT: {
		sc_packet_chat* my_packet = reinterpret_cast<sc_packet_chat*>(ptr);
		int o_id = my_packet->id;
		
		if (0 != npcs.count(o_id)) {
			npcs[o_id].add_chat_unicode(my_packet->mess);
			break;
		}

		chatting.add_chat_unicode(my_packet->mess);
		
	}
				   break;
	case S2C_STAT_CHANGE: {
		sc_packet_stat_change* pack = reinterpret_cast<sc_packet_stat_change*>(ptr);
		avatar.m_exp = pack->exp;
		avatar.m_level = pack->level;
		avatar.m_hp = pack->hp;
	}break;

	default:
		printf("Unknown PACKET type [%d]\n", ptr[1]);

	}
}

void process_data(char* net_buf, size_t io_byte)
{
	char* ptr = net_buf;
	static size_t in_packet_size = 0;
	static size_t saved_packet_size = 0;
	static char packet_buffer[BUF_SIZE];

	while (0 != io_byte) {
		if (0 == in_packet_size) in_packet_size = (int)static_cast<unsigned char>(ptr[0]);
		if (io_byte + saved_packet_size >= in_packet_size) {
			memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
			ProcessPacket(packet_buffer);
			ptr += in_packet_size - saved_packet_size;
			io_byte -= in_packet_size - saved_packet_size;
			in_packet_size = 0;
			saved_packet_size = 0;
		}
		else {
			memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
			saved_packet_size += io_byte;
			io_byte = 0;
		}
	}
}

void client_main()
{
	char net_buf[BUF_SIZE];
	size_t   received;

	auto recv_result = g_socket.receive(net_buf, BUF_SIZE, received);
	if (recv_result == sf::Socket::Error)
	{
		wcout << L"Recv 에러!";
		while (true);
	}

	if (recv_result == sf::Socket::Disconnected)
	{
		wcout << L"서버 접속 종료.";
		g_window->close();
	}

	if (recv_result != sf::Socket::NotReady)
		if (received > 0) process_data(net_buf, received);
	
	for (int i = 0; i < SCREEN_WIDTH; ++i) {
		int tile_x = i + g_left_x;
		if (tile_x >= WORLD_WIDTH) break;
		if (tile_x < 0) continue;
		for (int j = 0; j < SCREEN_HEIGHT; ++j)
		{
			int tile_y = j + g_top_y;
			if (tile_y < 0) continue;
			if (tile_y >= WORLD_HEIGHT) break;
			if (map_data[tile_y][tile_x]) {
				white_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				white_tile.a_draw();
			}
			else
			{
				black_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
				black_tile.a_draw();
			}
			//// if (((tile_x + tile_y) % 2) == 0) {
			//if (((tile_x / 3 + tile_y / 3) % 2) == 0) {
			//   white_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
			//   white_tile.a_draw();
			//}
			//else
			//{
			//   black_tile.a_move(TILE_WIDTH * i + 7, TILE_WIDTH * j + 7);
			//   black_tile.a_draw();
			//}
		}
	}
	avatar.draw();
	//   for (auto &pl : players) pl.draw();
	for (auto& npc : npcs) npc.second.draw();
	sf::Text text;
	text.setFont(g_font);
	text.setFillColor({ 0, 255, 255 });
	text.setStyle(sf::Text::Bold);
	char buf[100];
	sprintf_s(buf, "ID:%s HP:%d LEVEL:%d EXP:%d Pos:(%d, %d)", avatar.name, avatar.m_hp, avatar.m_level, avatar.m_exp, avatar.m_x, avatar.m_y);

	text.setString(buf);
	chatting.draw_chat();

	const wchar_t stext[] = { 0x4454, 0x263B, 0x2665, 0x2660, 0 };
	sf::Text chat(currentChat, g_font);
	chat.setFillColor({ 255, 255, 0 });
	chat.setStyle(sf::Text::Bold);
	chat.setPosition({ 0, WINDOW_HEIGHT * 2 - 50 });
	// chat.setString(text);
	g_window->draw(text);
	g_window->draw(chat);

}

void send_packet(void* packet)
{
	unsigned char* p = reinterpret_cast<unsigned char*>(packet);
	size_t sent;
	g_socket.send(p, p[0], sent);
}

void send_move_packet(unsigned char dir)
{
	cs_packet_move m_packet;
	m_packet.type = C2S_MOVE;
	m_packet.size = sizeof(m_packet);
	m_packet.direction = dir;
	send_packet(&m_packet);
}

void send_chat_packet(const wchar_t chat[])
{
	cs_packet_chat m_packet;
	m_packet.type = C2S_CHAT;
	m_packet.size = sizeof(cs_packet_chat);
	wcscpy_s(m_packet.message, chat);
	send_packet(&m_packet);
}

void send_attack_packet() {
	cs_packet_attack p;
	p.type = C2S_ATTACK;
	p.size = sizeof(p);
	send_packet(&p);
}


int main()
{
	read_map();
	wcout.imbue(locale("korean"));
	sf::Socket::Status status = g_socket.connect("127.0.0.1", SERVER_PORT);
	g_socket.setBlocking(false);

	if (status != sf::Socket::Done) {
		wcout << L"서버와 연결할 수 없습니다.\n";
		while (true);
	}

	client_initialize();

	/**************************************************/
	char name[20];
	cout << "ID를 입력하세요: ";
	cin >> name;
	/**************************************************/

	cs_packet_login l_packet;
	l_packet.size = sizeof(l_packet);
	l_packet.type = C2S_LOGIN;
	int t_id = GetCurrentProcessId();
	sprintf_s(l_packet.name, name);
	cout << l_packet.name << "입력\n";
	strcpy_s(avatar.name, l_packet.name);
	avatar.set_name(l_packet.name);
	send_packet(&l_packet);

	sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT");
	g_window = &window;

	sf::View view = g_window->getView();
	view.zoom(2.0f);
	view.move(SCREEN_WIDTH * TILE_WIDTH / 4, SCREEN_HEIGHT * TILE_WIDTH / 4);
	g_window->setView(view);

	bool isStartChat = false;

	while (window.isOpen())
	{
		sf::Event event;
		while (window.pollEvent(event))
		{

			if (event.type == sf::Event::Closed)
				window.close();
			if (event.type == sf::Event::TextEntered)
			{
				if (isStartChat) {
					if (event.text.unicode == 8)
						currentChat = currentChat.substr(0, currentChat.size() - 1);
					else
						currentChat += event.text.unicode;
				}
			}
			if (event.type == sf::Event::KeyPressed) {
				int p_type = -1;
				if (isStartChat)
				{
					if (event.key.code == sf::Keyboard::Enter)
					{
						isStartChat = false;
						currentChat = currentChat.substr(4, currentChat.size());
						// wcout << currentChat;
						send_chat_packet(currentChat.c_str());
						currentChat.clear();
					}
				}
				else
				{
					switch (event.key.code) {
					case sf::Keyboard::Left:
						send_move_packet(D_LEFT);
						break;
					case sf::Keyboard::Right:
						send_move_packet(D_RIGHT);
						break;
					case sf::Keyboard::Up:
						send_move_packet(D_UP);
						break;
					case sf::Keyboard::Down:
						send_move_packet(D_DOWN);
						break;
					case sf::Keyboard::Escape:
						window.close();
						break;
					case sf::Keyboard::A:
						send_attack_packet();
						break;
					case sf::Keyboard::Enter:
						currentChat += L"ME: ";
						isStartChat = true;
						break;
					}
				}
			}
		}

		window.clear();
		client_main();
		window.display();
	}
	client_finish();

	return 0;
}