/*
 * music.h — 简谱 MIDI 乐谱播放器
 * 平台: Windows (需链接 winmm.lib)
 *
 * 功能:
 *   - 解析简谱格式乐谱，通过 midiOutShortMsg 播放
 *   - 支持和弦 (方括号 [ ] 或花括号 { })
 *   - 支持音高标记: ^ 升八度, , 降八度, # 升半音（越界自动保护）
 *   - 支持时值标记: _ 半分, * 三分, & 七分, % 五分, . 附点, - 延音
 *   - 支持休止符 0
 *   - 双轨播放 (主旋律 + 伴奏)
 *   - 事件驱动模型: 每个音符 Note On/Off 配对，无无限延音；
 *     all_notes_off 发送正确的 All Notes Off (CC#123) 消息
 *   - 暂停/恢复/跳转/位置查询/总时长查询 (与 MCI 接口对齐)
 *   - 全局时间坐标系 (line_offset 行偏移)：事件比较统一为
 *     "全局时间 = line_offset + 行内 time_ms"，暂停/跳转后
 *     声音从正确位置继续，不会瞬间播完
 *   - 单次完整播放：乐谱播完一遍即结束（不无限循环）
 *   - timeGetTime() 精确计时, Sleep() 避免 100% CPU
 *   - std::atomic 多线程安全
 *
 * 乐谱文件格式 (music.txt):
 *   第一行: 基础拍速(毫秒)，如 500（该行剩余部分被忽略）
 *   后续行: 乐谱字符串，奇数行主旋律，偶数行伴奏
 *   示例:
 *     500
 *     1 2 3 4 | 5 - - -
 *     3 3 3 3 | 1 - - -
 *
 * 使用方法:
 *   BGM bgm("music.txt");   // 加载乐谱
 *   bgm.play();             // 开始播放(异步线程，播完一遍即停)
 *   bgm.pause();            // 暂停
 *   bgm.resume();           // 恢复
 *   bgm.seek(ms);           // 跳转到指定毫秒
 *   int pos = bgm.get_position();  // 当前位置(毫秒)
 *   int len = bgm.get_length();    // 总时长(毫秒)
 *   bgm.stop();             // 停止
 *
 * 编译: 需 -lwinmm (本文件已用 #pragma comment 自动链接)
 */

#pragma once
#include <Windows.h>
#include <thread>
#include <string.h>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <atomic>
#include <limits>

#pragma comment(lib, "winmm.lib")

class MusicList{
public:
	int dctn=500;
	std::vector <std::string> vec;
	~MusicList(){}
	void add(std::string s){
		vec.push_back(s);
	}
	void clear(){
		vec.clear();
	}
	void setDelay(int _dctn){
		dctn=_dctn;
	}
	void readFile(std::string fileName=""){
		clear();
		std::ifstream in(fileName);
		in>>dctn;
		/* 跳过第一行剩余部分（数字后的空格/制表符/换行），
		   避免 getline 读到空字符串加入 vec */
		in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
		std::string s;
		while (getline(in,s)) add(s);
		in.close();
	}
	MusicList(std::string fileName=""){
		vec.clear();
		if (fileName!="") readFile(fileName);
	}
};

/* 高效判断字符串是否为整数（可选正负号），替代低效的正则表达式 */
bool isNumeric(std::string const &str){
    if (str.empty()) return false;
    size_t i = 0;
    if (str[0] == '-' || str[0] == '+') i = 1;
    if (i >= str.size()) return false;
    for (; i < str.size(); i++) {
        if (str[i] < '0' || str[i] > '9') return false;
    }
    return true;
}

/* MIDI 事件：On=音符开, Off=音符关 */
struct MidiEvent {
	int time_ms;   /* 事件时间（毫秒） */
	int note;      /* 音高（0=休止符） */
	int type;      /* 0=Note Off, 1=Note On */
};

class MusicPlayer{
private:
	enum scale{
		Rest=0,
		C8=108,
		B7=107,A7s=106,A7=105,G7s=104,G7=103,F7s=102,F7=101,E7=100,D7s=99, D7=98, C7s=97, C7=96,
		B6=95, A6s=94, A6=93, G6s=92, G6=91, F6s=90, F6=89, E6=88, D6s=87, D6=86, C6s=85, C6=84,
		B5=83, A5s=82, A5=81, G5s=80, G5=79, F5s=78, F5=77, E5=76, D5s=75, D5=74, C5s=73, C5=72,
		B4=71, A4s=70, A4=69, G4s=68, G4=67, F4s=66, F4=65, E4=64, D4s=63, D4=62, C4s=61, C4=60,
		B3=59, A3s=58, A3=57, G3s=56, G3=55, F3s=54, F3=53, E3=52, D3s=51, D3=50, C3s=49, C3=48,
		B2=47, A2s=46, A2=45, G2s=44, G2=43, F2s=42, F2=41, E2=40, D2s=39, D2=38, C2s=37, C2=36,
		B1=35, A1s=34, A1=33, G1s=32, G1=31, F1s=30, F1=29, E1=28, D1s=27, D1=26, C1s=25, C1=24,
		B0=23, A0s=22, A0=21	
	};
	const int C_Scale[7][7]={{C1,D1,E1,F1,G1,A1,B1},
							 {C2,D2,E2,F2,G2,A2,B2},
							 {C3,D3,E3,F3,G3,A3,B3},
							 {C4,D4,E4,F4,G4,A4,B4},
							 {C5,D5,E5,F5,G5,A5,B5},
							 {C6,D6,E6,F6,G6,A6,B6},
							 {C7,D7,E7,F7,G7,A7,B7}};
	const int C_Scale_s[7][7]={{C1s,D1s,-1,F1s,G1s,A1s,-1},
							   {C2s,D2s,-1,F2s,G2s,A2s,-1},
							   {C3s,D3s,-1,F3s,G3s,A3s,-1},
							   {C4s,D4s,-1,F4s,G4s,A4s,-1},
							   {C5s,D5s,-1,F5s,G5s,A5s,-1},
							   {C6s,D6s,-1,F6s,G6s,A6s,-1},
							   {C7s,D7s,-1,F7s,G7s,A7s,-1}};
	HMIDIOUT handle;
	int dctn=500;
	int volume=0x7f;
	/* 当前行在整首乐谱中的全局起始毫秒（playList 逐行累计）。
	   事件 time_ms 是行内相对值（每行从 0 开始），与全局位置
	   （current_ms / seek）比较时必须加此偏移，否则跳转后
	   行内事件被误判为"已过期"而瞬间全部播完（听感为无声音） */
	int line_offset = 0;

	/* 解析一行乐谱，生成 MIDI 事件列表（On + Off 配对） */
	void parse_line(const std::string &s, std::vector<MidiEvent> &events, int &end_ms) {
		std::string str = s + ' ';
		int n = str.size();
		int ctn = 32 * 21;
		bool isChord = false;
		std::vector<int> notes;  /* 当前和弦的音高列表 */
		int cur_time = 0;
		double ms_per_tick = dctn / 32.0 / 21.0;

		for (int i = 0; i < n; ++i) {
			char c = str[i];
			switch (c) {
				case '[': case '{':
					isChord = true;
					break;
				case ']': case '}':
					isChord = false;
					break;
				case ' ': {
					if (!isChord && !notes.empty()) {
						int dur = (int)(ctn * ms_per_tick);
						/* 发送所有音符的 Note On */
						for (int nt : notes) {
							if (nt != 0) {
								MidiEvent ev;
								ev.time_ms = cur_time;
								ev.note = nt;
								ev.type = 1; /* On */
								events.push_back(ev);
							}
						}
						/* 发送所有音符的 Note Off */
						for (int nt : notes) {
							if (nt != 0) {
								MidiEvent ev;
								ev.time_ms = cur_time + dur;
								ev.note = nt;
								ev.type = 0; /* Off */
								events.push_back(ev);
							}
						}
						cur_time += dur;
						notes.clear();
					}
					ctn = 32 * 21;
					break;
				}
				case '|': break;
				case '_': ctn /= 2; break;
				case '*': ctn /= 3; break;
				case '&': ctn /= 7; break;
				case '%': ctn /= 5; break;
				case '.': ctn = (int)(ctn * 1.5); break;
				case '-': ctn += 32 * 21; break;
				case '0':
					notes.push_back(0); /* 休止符 */
					break;
				default: {
					if (c >= '1' && c <= '7') {
						int x = (int)c - 49, lvl = 3;
						bool isSharp = false;
						for (int j = i + 1; j < n; ++j) {
							if (str[j] == '^') lvl++;
							else if (str[j] == ',') lvl--;
							else if (str[j] == '#') isSharp = true;
							else break;
							i++;
						}
						if (lvl >= 0 && lvl <= 6 && x >= 0 && x <= 6) {
							int note = isSharp ? C_Scale_s[lvl][x] : C_Scale[lvl][x];
							if (note > 0) notes.push_back(note);
						}
					}
					break;
				}
			}
		}
		end_ms = cur_time;
	}

public:
	std::atomic<int>  STOP{0};
	std::atomic<bool> ENDMUSIC{false};
	std::atomic<bool> PAUSED{false};
	std::atomic<int>  SEEK_MS{-1};   /* >=0 时跳转到指定位置 */
	std::atomic<int>  current_ms{0}; /* 当前播放位置（毫秒） */
	int total_ms = 0;                /* 总时长（毫秒） */
	/* 当前行在整首乐谱中的全局起始毫秒（playList 逐行累计） */
	int get_line_offset(){ return line_offset; }

	MusicPlayer(){
		midiOutOpen(&handle,0,0,0,CALLBACK_NULL);
	}
	~MusicPlayer(){
		all_notes_off();
		midiOutClose(handle);
	}
	void setVolume(int _vol){
		volume=_vol;
	}
	void setDelay(int _dctn){
		dctn=_dctn;
	}

	/* 立即关闭所有正在播放的音符（防止音符无限延音）
	   消息格式：data2 << 16 | data1 << 8 | status
	   All Notes Off = CC#123（data1=0x7B），status = 0xB0 | ch */
	void all_notes_off() {
		for (int ch = 0; ch < 16; ch++) {
			midiOutShortMsg(handle, (0 << 16) | (0x7B << 8) | (0xB0 | ch));
		}
	}

	/* 播放事件列表（带暂停/跳转/停止控制，使用 Sleep 避免 100% CPU）
	   事件比较统一用"全局时间 = line_offset + time_ms" */
	void play_events(const std::vector<MidiEvent> &events) {
		size_t idx = 0;
		int start_time = timeGetTime();
		int base_ms = current_ms.load();

		while (idx < events.size()) {
			if (ENDMUSIC.load()) { all_notes_off(); return; }

			/* 检查跳转请求 */
			int seek = SEEK_MS.load();
			if (seek >= 0) {
				/* 相对当前行的位置；若 seek 超出本行范围（含行内末尾），
				   跳过本行把 seek 留给下一行处理 */
				int rel = seek - line_offset;
				if (rel < 0) rel = 0;
				int line_end = (int)events.size();
				int last_t = (line_end > 0) ? events[line_end - 1].time_ms : 0;
				if (rel > last_t) {
					all_notes_off();
					idx = events.size();  /* 跳过本行，退出 play_events，playList 进入下一行 */
					continue;
				}
				SEEK_MS.store(-1);
				all_notes_off();
				base_ms = seek;
				start_time = timeGetTime();
				current_ms.store(seek);
				/* 定位到第一个全局时间 >= seek 的事件 */
				idx = 0;
				while (idx < events.size() && line_offset + events[idx].time_ms < seek) idx++;
				continue;
			}

			/* 暂停处理 */
			if (PAUSED.load()) {
				all_notes_off();
				while (PAUSED.load() && !ENDMUSIC.load()) {
					if (SEEK_MS.load() >= 0) break;
					Sleep(20);
				}
				if (ENDMUSIC.load()) return;
				/* 恢复：重置时间基准（current_ms 是全局位置，暂停期间保持） */
				start_time = timeGetTime();
				base_ms = current_ms.load();
				continue;
			}

			int elapsed = timeGetTime() - start_time + base_ms;
			current_ms.store(elapsed);

			if (line_offset + events[idx].time_ms <= elapsed) {
				const MidiEvent &ev = events[idx];
				if (ev.type == 1) {
					/* Note On */
					midiOutShortMsg(handle, (volume << 16) | (ev.note << 8) | 0x90);
				} else {
					/* Note Off */
					midiOutShortMsg(handle, (0 << 16) | (ev.note << 8) | 0x80);
				}
				idx++;
			} else {
				/* 睡到下一个事件，避免忙等待 */
				int wait_ms = (line_offset + events[idx].time_ms) - elapsed;
				if (wait_ms > 50) wait_ms = 50;
				if (wait_ms < 1) wait_ms = 1;
				Sleep(wait_ms);
			}
		}
		all_notes_off();
	}

	void play(std::string s1,std::string s2=""){
		STOP.store(0);
		/* 解析两行乐谱，合并事件 */
		std::vector<MidiEvent> events;
		int end1 = 0, end2 = 0;
		if (!s1.empty()) parse_line(s1, events, end1);
		if (!s2.empty()) parse_line(s2, events, end2);
		/* 事件已按时间顺序生成，无需排序 */
		total_ms = (end1 > end2) ? end1 : end2;
		play_events(events);
		STOP.store(1);
	}

	void playList(MusicList &m){
		dctn=m.dctn;
		ENDMUSIC.store(false);
		line_offset = 0;  /* 从头播放：第一行全局起始为 0 */
		for (int i=0;i<(int)m.vec.size() && !ENDMUSIC.load();++i){
			/* 内层 while 也检查 ENDMUSIC，确保 stop() 后能及时退出 */
			while (i<(int)m.vec.size() && !ENDMUSIC.load() && (m.vec[i]=="" || isNumeric(m.vec[i]))){
				if (isNumeric(m.vec[i])) {
					/* stoi 对超长数字会抛异常，用 try/catch 保护；超过 int 范围时设为默认值 */
					try { setDelay(stoi(m.vec[i])); }
					catch (...) { setDelay(500); }
				}
				i++;
			}
			if (i>=(int)m.vec.size() || ENDMUSIC.load()) break;
			std::string s1=m.vec[i],s2="";
			if (i<(int)m.vec.size()-1 && m.vec[i+1]!="") s2=m.vec[i+1],i++;
			play(s1,s2);
			/* 本行结束：下一行从本行末尾的全局时间开始 */
			line_offset += total_ms;
		}
	}
};

class BGM{
public:
	MusicPlayer player;
	MusicList nowList;
	std::thread *bgm_thread = nullptr;
	std::atomic<bool> thread_running{false};

	BGM(std::string name,int volume=0x7f){
		nowList.readFile(name);player.setVolume(volume);
	}
	~BGM(){
		stop();
	}
	void setMusic(std::string name){
		nowList.readFile(name);
	}
	void play_thread(){
		thread_running.store(true);
		/* 只完整播放一遍乐谱：playList 返回后歌曲即结束，
		   置 ENDMUSIC=true 退出线程，避免无限循环重播 */
		if (!nowList.vec.empty()) player.playList(nowList);
		player.ENDMUSIC.store(true);
		thread_running.store(false);
	}
	void play(){
		stop();
		player.ENDMUSIC.store(false);
		player.PAUSED.store(false);
		player.current_ms.store(0);
		/* 先设置 thread_running=true，避免 stop() 在 detach 后、线程启动前误判 */
		thread_running.store(true);
		bgm_thread = new std::thread(&BGM::play_thread, this);
		bgm_thread->detach();
	}
	void stop(){
		player.ENDMUSIC.store(true);
		player.PAUSED.store(false);
		player.all_notes_off();
		/* 等待线程结束（最多等 1000ms，给高负载系统留余量） */
		for (int i = 0; i < 100 && thread_running.load(); i++) Sleep(10);
		/* 线程已 detach，delete 仅释放 std::thread 对象的堆内存，不等待 OS 线程 */
		if (bgm_thread) { delete bgm_thread; bgm_thread = nullptr; }
	}
	/* 暂停 */
	void pause(){
		player.PAUSED.store(true);
	}
	/* 恢复播放 */
	void resume(){
		player.PAUSED.store(false);
	}
	/* 跳转到指定毫秒位置 */
	void seek(int ms){
		if (ms < 0) ms = 0;
		player.current_ms.store(ms);
		player.SEEK_MS.store(ms);
		/* 如果暂停中，先关闭所有音符 */
		player.all_notes_off();
	}
	/* 获取当前播放位置（毫秒） */
	/* 获取当前播放位置（毫秒）
	   注意：当前 start.cpp 未调用此函数，保留作为公共 API 供外部使用 */
	int get_position(){
		return player.current_ms.load();
	}
	/* 获取总时长（毫秒）= 已累计的行偏移 + 当前行时长（playList 播放完后的值即整首乐谱总时长） */
	int get_length(){
		return player.get_line_offset() + player.total_ms;
	}
	/* 是否暂停 */
	bool is_paused(){
		return player.PAUSED.load();
	}
};
