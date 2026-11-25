#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>

using namespace std;

template <class T>
class BoundedBuffer {
    queue<T> records;
    unsigned long long capacity;
    bool closed = false;
    mutex bufferMutex;
    condition_variable notEmpty, notFull;

public:
    explicit BoundedBuffer(unsigned long long size) : capacity(size) {}

    void push(T record) {
        unique_lock<mutex> lock(bufferMutex);
        notFull.wait(lock, [&] { return records.size() < capacity; });
        records.push(std::move(record));
        lock.unlock();
        notEmpty.notify_one();
    }

    bool pop(T& record) {
        unique_lock<mutex> lock(bufferMutex);
        notEmpty.wait(lock, [&] { return !records.empty() || closed; });
        if (records.empty()) return false;
        record = std::move(records.front());
        records.pop();
        lock.unlock();
        notFull.notify_one();
        return true;
    }

    void close() {
        {
            lock_guard<mutex> lock(bufferMutex);
            closed = true;
        }
        notEmpty.notify_all();
    }
};

struct Line {
    string filename;
    unsigned long long number = 0;
    string text;
};

unsigned long long number(const string& text, unsigned long long maximum) {
    if (text.empty()) throw invalid_argument("empty number");
    unsigned long long value = 0;
    for (char ch : text) {
        if (ch < '0' || ch > '9')
            throw invalid_argument("expected nonnegative integer or -1 filter");
        int digit = ch - '0';
        if (value > maximum / 10 ||
            (value == maximum / 10 && static_cast<unsigned long long>(digit) > maximum % 10))
            throw out_of_range("numeric argument out of range");
        value = value * 10 + digit;
    }
    return value;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        cerr << "Usage: ./pipegrep buffsize filesize uid gid \"string\"\n";
        return 1;
    }

    unsigned long long capacity;
    bool useSize = string(argv[2]) != "-1";
    bool useUid = string(argv[3]) != "-1";
    bool useGid = string(argv[4]) != "-1";
    unsigned long long minimumSize = 0, owner = 0, group = 0;
    string substring = argv[5];
    try {
        capacity = number(argv[1], numeric_limits<size_t>::max());
        if (capacity == 0) throw invalid_argument("buffsize must be positive");
        if (useSize) minimumSize = number(argv[2], numeric_limits<off_t>::max());
        if (useUid) owner = number(argv[3], numeric_limits<uid_t>::max());
        if (useGid) group = number(argv[4], numeric_limits<gid_t>::max());
        if (substring.empty()) throw invalid_argument("search string must not be empty");
    } catch (const exception& error) {
        cerr << "Invalid arguments: " << error.what() << '\n';
        return 1;
    }

    atomic<bool> hadError{false};
    mutex errorMutex;
    auto report = [&](const string& message) {
        hadError.store(true);
        lock_guard<mutex> lock(errorMutex);
        cerr << "pipegrep: " << message << '\n';
    };
    auto fatal = [&](const string& message) {
        report(message);
        exit(EXIT_FAILURE);
    };

    try {
        BoundedBuffer<string> b1(capacity), b2(capacity);
        BoundedBuffer<Line> b3(capacity), b4(capacity);
        auto acquire = [&] {
            DIR* directory = opendir(".");
            if (!directory) fatal(string("cannot open current directory: ") + strerror(errno));
            while (true) {
                errno = 0;
                dirent* entry = readdir(directory);
                if (!entry) {
                    int error = errno;
                    closedir(directory);
                    if (error) fatal(string("cannot enumerate directory: ") + strerror(error));
                    break;
                }
                string name = entry->d_name;
                if (name != "." && name != "..") b1.push("./" + name);
            }
            b1.close();
        };
        auto filterFiles = [&] {
            string path;
            while (b1.pop(path)) {
                struct stat info{};
                if (lstat(path.c_str(), &info) != 0) {
                    report(path + ": cannot inspect: " + strerror(errno));
                    continue;
                }
                if (!S_ISREG(info.st_mode)) continue;
                if (useSize && (info.st_size < 0 ||
                    static_cast<unsigned long long>(info.st_size) <= minimumSize)) continue;
                if (useUid && static_cast<unsigned long long>(info.st_uid) != owner) continue;
                if (useGid && static_cast<unsigned long long>(info.st_gid) != group) continue;
                b2.push(std::move(path));
            }
            b2.close();
        };
        auto generateLines = [&] {
            string path;
            while (b2.pop(path)) {
                ifstream file(path);
                if (!file.is_open()) {
                    report(path + ": cannot open file");
                    continue;
                }
                string text;
                unsigned long long lineNumber = 0;
                while (getline(file, text)) {
                    ++lineNumber;
                    b3.push(Line{path, lineNumber, std::move(text)});
                }
                if (file.bad() || (file.fail() && !file.eof()))
                    report(path + ": cannot read file");
            }
            b3.close();
        };
        auto filterLines = [&] {
            Line line;
            while (b3.pop(line)) {
                if (line.text.find(substring) != string::npos)
                    b4.push(std::move(line));
            }
            b4.close();
        };
        auto output = [&] {
            Line line;
            unsigned long long count = 0;
            while (b4.pop(line)) {
                cout << line.filename << '(' << line.number << "): " << line.text << '\n';
                ++count;
            }
            cout << "***** You found " << count << " matches *****\n";
            cout.flush();
            if (!cout) report("cannot write standard output");
        };
        auto run = [&](auto stage) {
            try { stage(); }
            catch (const exception& error) { fatal(error.what()); }
            catch (...) { fatal("unrecoverable worker failure"); }
        };
        thread workers[5];
        try {
            workers[0] = thread([&] { run(acquire); });
            workers[1] = thread([&] { run(filterFiles); });
            workers[2] = thread([&] { run(generateLines); });
            workers[3] = thread([&] { run(filterLines); });
            workers[4] = thread([&] { run(output); });
        } catch (const exception& error) {
            fatal(string("cannot create worker: ") + error.what());
        }
        for (auto& worker : workers) worker.join();
    } catch (const exception& error) {
        report(error.what());
        return 1;
    }
    return hadError.load() ? 1 : 0;
}
