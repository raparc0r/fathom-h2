#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
"""
A copy of the simulator's library with every other artist, album and song given
a made-up Japanese (or, with "ko", "zh-hans" or "zh-hant" as a third word,
Korean or Chinese) name, to look at a character pack on every screen:

    python3 tools/fonts/sim_names.py build/sim-data/library.db build/sim-data-ja/library.db [ja|ko|zh-hans|zh-hant] [wide]
    FATHOM_SIM_DB=build/sim-data-ja/library.db build/sim/fathom-sim --screenshots DIR

With "wide" the names aren't words but runs of the language's everyday characters
taken at random (the same every time): the 2,965 kanji of JIS level 1 with a
kana or two, the 2,350 syllables of KS X 1001, or the everyday characters of
GB2312 (3,755) or Big5 (5,401). Nothing to read, but every
name brings characters the last one didn't have, as a real library's do far
more than the made-up names: for measuring on the H2 how a pack's pages come in.

Only the names shown change (their places in the lists stay where the old names
put them). The names are invented: everyday words, no real artist or song.
"""
import os
import sqlite3
import sys

ARTISTS = ['山田太郎', '佐藤花子', '高橋バンド', 'ザ・サンプルズ', '鈴木一郎トリオ', '東京サウンドシステム', 'あおぞら', 'ｻﾝﾌﾟﾙ楽団',
           '髙橋と﨑山', '北海道オーケストラ', 'みどりの森', '夜行列車']
ALBUMS = ['東京の夜', '春の歌', '夏の思い出', '青い空の下で', '雨の日曜日', '海辺のカフェ', '月明かりの道', '朝の散歩', '夜のドライブ',
          '遠い町から', '白い雪', '小さな庭', '古い写真', '赤い自転車', '静かな湖', '秋の夕暮れ', 'ベスト・コレクション １９９８－２００８',
          'ライブ・イン・大阪', '第一集「はじまり」']
SONGS = ['はじめに', '窓の外', '駅までの道', '五月の風', 'コーヒーをもう一杯', '眠れない夜', '坂の上の家', '川沿いの桜', '手紙',
         '電話のベル', '土曜日の午後', '帰り道', '君の名前を呼ぶ', '長い冬のあとで', 'ｲﾝﾀｰﾙｰﾄﾞ', '終わりに（リプライズ）', '♪ おやすみ',
         'とても長い題名の歌、最後まで読めるかどうかを確かめるために', '一二三', '星空']


KO = {
    'artists': ['김민준', '이서연', '박지훈 밴드', '서울 사운드 시스템', '파란하늘', '최유진 트리오', '한강 오케스트라', '초록숲',
                '야간열차', '더 샘플즈', '정하은과 친구들', '부산 재즈 클럽'],
    'albums': ['서울의 밤', '봄의 노래', '여름의 추억', '푸른 하늘 아래', '비 오는 일요일', '바닷가 카페', '달빛 길', '아침 산책',
               '밤의 드라이브', '먼 도시에서', '하얀 눈', '작은 정원', '오래된 사진', '빨간 자전거', '고요한 호수', '가을 저녁',
               '베스트 컬렉션 １９９８－２００８', '라이브 인 부산', '제１집 「시작」'],
    'songs': ['처음에', '창밖', '역까지 가는 길', '오월의 바람', '커피 한 잔 더', '잠 못 드는 밤', '언덕 위의 집', '강가의 벚꽃', '편지',
              '전화벨', '토요일 오후', '집으로 가는 길', '너의 이름을 부르며', '긴 겨울이 지나고', '간주곡', '끝으로 (리프라이즈)',
              '♪ 잘 자요', '끝까지 읽을 수 있는지 확인하기 위한 아주 아주 긴 제목의 노래', 'ㅋㅋㅋ', '별하늘'],
}


SC = {
    'artists': ['张伟', '王芳', '李娜乐队', '北京之声', '蓝天', '刘洋三重奏', '长江交响乐团', '绿森林', '夜行列车', '样本乐队',
                '陈静和朋友们', '上海爵士俱乐部'],
    'albums': ['北京的夜晚', '春天的歌', '夏天的回忆', '在蓝天下', '下雨的星期天', '海边咖啡馆', '月光小路', '清晨散步',
               '夜间兜风', '来自远方的城市', '白雪', '小花园', '旧照片', '红色自行车', '宁静的湖', '秋天的黄昏',
               '精选集 １９９８－２００８', '上海现场', '第一辑「开始」', '真直骨角令海道写画返'],
    'songs': ['开始', '窗外', '去车站的路', '五月的风', '再来一杯咖啡', '失眠的夜', '山坡上的房子', '河边的樱花', '信',
              '电话铃声', '星期六下午', '回家的路', '呼唤你的名字', '漫长的冬天之后', '间奏曲', '结束（重奏）',
              '♪ 晚安', '为了确认能否读到最后而写的一首非常非常长的歌名', '一二三', '星空'],
}
TC = {
    'artists': ['陳怡君', '林志明', '張雅婷樂團', '臺北之聲', '藍天', '王俊傑三重奏', '淡水河交響樂團', '綠森林', '夜行列車',
                '樣本樂隊', '黃淑芬和朋友們', '高雄爵士俱樂部'],
    'albums': ['臺北的夜晚', '春天的歌', '夏天的回憶', '在藍天下', '下雨的星期天', '海邊咖啡館', '月光小路', '清晨散步',
               '夜間兜風', '來自遠方的城市', '白雪', '小花園', '舊照片', '紅色腳踏車', '寧靜的湖', '秋天的黃昏',
               '精選集 １９９８－２００８', '高雄現場', '第一輯「開始」', '真直骨角令海道寫畫返'],
    'songs': ['開始', '窗外', '去車站的路', '五月的風', '再來一杯咖啡', '失眠的夜', '山坡上的房子', '河邊的櫻花', '信',
              '電話鈴聲', '星期六下午', '回家的路', '呼喚你的名字', '漫長的冬天之後', '間奏曲', '結束（重奏）',
              '♪ 晚安', '為了確認能否讀到最後而寫的一首非常非常長的歌名', '一二三', '星空'],
}


def wide(lang):
    """Three pools of names made of the language's everyday characters at random."""
    import random
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkpack
    r = random.Random(20261126)
    if lang == 'ko':
        common = [u for u in mkpack.korean()[:3400] if 0xAC00 <= u <= 0xD7A3]
        word = lambda: ''.join(chr(r.choice(common)) for _ in range(r.randint(2, 4)))
        name = lambda n: ' '.join(word() for _ in range(n))
    elif lang in ('zh-hans', 'zh-hant'):
        chars = mkpack.chinese_simplified()[:4300] if lang == 'zh-hans' else mkpack.chinese_traditional()[:5800]
        common = [u for u in chars if 0x4E00 <= u <= 0x9FFF]
        name = lambda n: ''.join(''.join(chr(r.choice(common)) for _ in range(r.randint(2, 4))) + (' ' if k < n - 1 else '') for k in range(n))
    else:
        common = [u for u in mkpack.japanese()[:3700] if 0x4E00 <= u <= 0x9FFF]
        kana = [chr(u) for u in range(0x3042, 0x3094)]
        name = lambda n: ''.join(''.join(chr(r.choice(common)) for _ in range(r.randint(1, 3))) + r.choice(kana) for _ in range(n))
    return [name(1) for _ in range(120)], [name(2) for _ in range(200)], [name(r.randint(2, 3)) for _ in range(900)], len(common)


def main():
    src, dst = sys.argv[1], sys.argv[2]
    global ARTISTS, ALBUMS, SONGS
    lang = sys.argv[3] if len(sys.argv) > 3 else 'ja'
    pools = {'ko': KO, 'zh-hans': SC, 'zh-hant': TC}.get(lang)
    if pools:
        ARTISTS, ALBUMS, SONGS = pools['artists'], pools['albums'], pools['songs']
    if 'wide' in sys.argv[4:]:
        ARTISTS, ALBUMS, SONGS, pool = wide(lang)
        used = len(set(''.join(ARTISTS + ALBUMS + SONGS)) - set(' '))
        print('wide: %d different characters in the names, of the %d everyday ones' % (used, pool))
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
    for ext in ('', '-wal', '-shm'):
        if os.path.exists(dst + ext):
            os.remove(dst + ext)
    a = sqlite3.connect('file:%s?mode=ro' % src, uri=True)
    b = sqlite3.connect(dst)
    a.backup(b)
    a.close()
    artists = {}
    for (i,) in b.execute('SELECT id FROM artists ORDER BY sort').fetchall()[::2]:
        artists[i] = ARTISTS[len(artists) % len(ARTISTS)]
        b.execute('UPDATE artists SET name=? WHERE id=?', (artists[i], i))
    albums = {}
    for (i,) in b.execute('SELECT id FROM albums ORDER BY sort').fetchall()[::2]:
        albums[i] = ALBUMS[len(albums) % len(ALBUMS)]
        b.execute('UPDATE albums SET title=? WHERE id=?', (albums[i], i))
    n = 0
    for i, album_id, artist_id in b.execute('SELECT id, album_id, artist_id FROM tracks ORDER BY album_id, disc, track').fetchall():
        if album_id in albums:
            b.execute('UPDATE tracks SET album=? WHERE id=?', (albums[album_id], i))
        if artist_id in artists:
            b.execute('UPDATE tracks SET artist=?, album_artist=? WHERE id=?', (artists[artist_id], artists[artist_id], i))
        if i % 2:
            b.execute('UPDATE tracks SET title=? WHERE id=?', (SONGS[n % len(SONGS)], i))
            n += 1
    b.commit()
    b.close()
    print('%s: %d artists, %d albums, %d songs renamed' % (dst, len(artists), len(albums), n))


if __name__ == '__main__':
    main()
