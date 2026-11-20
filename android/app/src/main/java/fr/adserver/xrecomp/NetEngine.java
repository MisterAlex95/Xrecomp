package fr.adserver.xrecomp;

import java.io.File;
import java.io.IOException;

/**
 * The BitTorrent session the network library drives (XBA-321), behind an interface so the JVM tests drive NetRunner
 * with a fake: production is LibtorrentEngine (libtorrent4j), the only class that touches libtorrent. Torrents are
 * named by their v1 info hash (40 hex digits). Not thread-safe: NetService calls it from its one worker thread
 * (metadata fetches from a second one, which only call {@link #fetchMetadata}).
 */
interface NetEngine {
    /** Starts the session (idempotent). */
    void start() throws IOException;

    /** Ends the session: every torrent leaves it, files stay. */
    void stop();

    /**
     * The metadata of a magnet link (a whole .torrent or a bare info dictionary, as TorrentMeta.parse /
     * parseInfo read them), fetched from its peers; null when none answered within timeoutSec.
     */
    byte[] fetchMetadata(String magnet, int timeoutSec) throws IOException;

    /** The torrent is in the session. */
    boolean has(String id);

    /**
     * Adds a torrent (its .torrent bytes) saving under saveDir, every file at priority 0 (nothing downloads, nothing
     * is created) and paused. A torrent already in the session is left as it is.
     */
    void add(String id, byte[] torrent, File saveDir) throws IOException;

    /**
     * Downloads exactly these files (TorrentMeta.File.index, checked against the engine's own file list by path and
     * size: a mismatch throws, nothing is set), every other at priority 0; resumes the torrent. An empty list pauses it.
     */
    void want(String id, int[] fileIndexes, String[] paths, long[] sizes) throws IOException;

    /** Bytes downloaded and checked, per file of the torrent (TorrentMeta.File.index), or null when it is not in. */
    long[] fileProgress(String id);

    /** The payload download rate, bytes a second. */
    int downloadRate(String id);

    /** Peers connected now. */
    int peers(String id);

    /** The torrent's error (a disk full, a folder gone), or null. */
    String error(String id);

    /** Connects the torrent to a peer the player named (their own NAS running a client, say; NET-4's « Add a peer »). */
    void connectPeer(String id, String host, int port) throws IOException;

    /** Asks for the torrent's resume data (what came, checked), written when libtorrent hands it over. */
    void saveResume(String id);

    /** Deletes the torrent's resume data (its files were deleted, or its source removed). */
    void forgetResume(String id);

    /** Takes the torrent out of the session (files stay) and forgets its resume data. */
    void remove(String id);
}
