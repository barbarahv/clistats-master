/* ============================================================================
 * ARQUIVO: so_threads.cpp
 *
 * VERSÃO PARALELA COM THREADS (std::thread + memória compartilhada + mutex)
 * — NÚMERO DE THREADS DEFINIDO NA EXECUÇÃO (-p N / --processes N).
 *
 * Segue a MESMA BASE de so_sequencial.cpp (mesmas classes, mesmo parser,
 * mesmo Welford, mesma saída). O que muda é a divisão do arquivo em N
 * pedaços de bytes, processados por N threads.
 *
 *   - O arquivo é dividido em N fatias (cortes no meio de uma linha).
 *   - Cada thread processa sua fatia: linhas que COMEÇAM na fatia
 *     pertencem a ela (mesma regra do so_processos.cpp).
 *   - O parsing é feito SEM lock (variáveis locais da thread).
 *   - A atualização das estatísticas é feita DENTRO de seção crítica
 *     (mutex global compartilhado), uma thread por vez.
 *   - A thread principal só espera (join) e lê o resultado.
 *
 * Fallback sequencial (mesmo código do so_sequencial) quando:
 *   - a entrada é stdin (não dá para dividir um fluxo);
 *   - -fd está ativo (a saída precisa sair em ordem);
 *   - -k está ativo (contagem global de linhas);
 *   - arquivo pequeno (menos de algumas linhas por thread).
 *
 * Exemplo:  ./so_threads -i dados.csv -s 1 -fc 1,2,3,4 -p 4
 * ============================================================================ */

/* Cabeçalhos padrão da biblioteca C++ */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>   // getpid()


struct ApplicationProperties
{
    static std::string version()
    {
        return std::string(VERSION_MAJOR) + "." + VERSION_MINOR + "." + VERSION_PATCH;
    }
    static constexpr const char * NAME          = "clistats";
    static constexpr const char * AUTHOR        = "dpmcmlxxvi@gmail.com";
    static constexpr const char * VERSION_MAJOR = "1";
    static constexpr const char * VERSION_MINOR = "1";
    static constexpr const char * VERSION_PATCH = "0";
};

struct Logger
{
    struct Level {
        enum Type { FATAL = 0, ERROR, WARNING, INFO, DEBUG, DETAIL };
    };

    static std::mutex & mutex()
    {
        static std::mutex m;
        return m;
    }

    static std::ostream & log(Logger::Level::Type level)
    {
        std::string label = (level==Level::FATAL  ? "FATAL":
                            (level==Level::ERROR  ? "ERROR":
                            (level==Level::WARNING? "WARNING":
                            (level==Level::INFO   ? "INFO":
                            (level==Level::DEBUG  ? "DEBUG":"DETAIL")))));
        std::ostream * os = (level <= Level::WARNING ? &std::cerr : &std::cout);
        *os << std::left << std::setw(7) << label << ": ";
        return *os;
    }

    static Level::Type logLevel;
};
Logger::Level::Type Logger::logLevel = Logger::Level::FATAL;

class LogStream
{
public:
    explicit LogStream(Logger::Level::Type level) :
        _enabled(level <= Logger::logLevel), _stream(0)
    {
        if (_enabled) {
            Logger::mutex().lock();
            _stream = &Logger::log(level);
        }
    }

    LogStream(LogStream && other) noexcept :
        _enabled(other._enabled), _stream(other._stream)
    {
        other._enabled = false;
        other._stream = 0;
    }

    LogStream(LogStream const &) = delete;
    LogStream & operator=(LogStream const &) = delete;

    ~LogStream()
    {
        if (_enabled) {
            *_stream << std::endl;
            Logger::mutex().unlock();
        }
    }

    template <class T>
    LogStream & operator<<(T const & value)
    {
        if (_enabled) *_stream << value;
        return *this;
    }

private:
    bool _enabled;
    std::ostream * _stream;
};

inline LogStream logMessage(Logger::Level::Type level)
{
    return LogStream(level);
}


class StringParser
{
public:
    template <class T>
    static std::string parseNumber(T value)
    {
        if (std::isnan(static_cast<double>(value))) return "nan";
        std::string entry;
        std::stringstream parser;
        parser << std::fixed << value;
        parser >> entry;
        return entry;
    }

    template <class T>
    static bool toValue(std::string const & src, T & dst)
    {
        std::istringstream parser(src.c_str());
        parser >> dst;
        if (!parser) return false;
        return (parser.rdbuf()->in_avail() == 0);
    }
};

template <>
inline bool StringParser::toValue<double>(std::string const & src, double & dst)
{
    if (src.empty()) return false;
    for (char c : src) {
        if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' ||
              c == 'e' || c == 'E' || c == ' ' || c == '\t'))
            return false;
    }
    char * end = 0;
    const char * begin = src.c_str();
    double value = std::strtod(begin, &end);
    if (end == begin || *end != '\0') return false;
    dst = value;
    return true;
}


class StringSplitter
{
public:
    typedef std::vector<std::string>::size_type size_type;

    StringSplitter() {}
    StringSplitter(std::string const & src, std::string const & delim)
    { initialize(src, delim); }

    std::string const & at(size_type index) const { return _tokens.at(index); }

    void initialize(std::string const & src, std::string const & delim)
    {
        std::vector<std::string> tokens;
        std::string::size_type startpos = 0, endpos = 0;
        while (true) {
            endpos = src.find(delim, startpos);
            tokens.push_back(src.substr(startpos, endpos - startpos));
            if (endpos == std::string::npos) break;
            startpos = endpos + delim.size();
        }
        _tokens.swap(tokens);
    }

    size_type size() const { return _tokens.size(); }
    std::vector<std::string> tokens() const { return _tokens; }

    template <class T>
    bool toValue(size_type index, T & dst) const
    { return StringParser::toValue<T>(at(index), dst); }

    static bool toIntegers(std::string const & src, std::vector<int> & dst,
                           std::string const & delim = ":")
    {
        int start = 0, stop = 0;
        StringSplitter splitter(src, delim);
        if (splitter.size() == 1) {
            if (!StringParser::toValue<int>(splitter.at(0), start)) return false;
            stop = start;
        } else if (splitter.size() == 2) {
            if (!StringParser::toValue<int>(splitter.at(0), start)) return false;
            if (!StringParser::toValue<int>(splitter.at(1), stop))  return false;
            if (stop < start) std::swap(start, stop);
        } else return false;
        for (int i = start; i < stop + 1; i++) dst.push_back(i);
        return true;
    }

private:
    std::vector<std::string> _tokens;
};


struct DataPoint
{
    DataPoint() : active(false), value(0) {}
    bool active;
    double value;
};

class DataVector : public std::vector<DataPoint>
{
public:
    void deactivate()
    { for (DataPoint & point : *this) point.active = false; }

    static void copy(DataVector const & src,
                     DataVector::size_type const & numSrc,
                     DataVector & dst,
                     DataVector::size_type const & numDst,
                     std::vector<bool> const & mask,
                     DataVector::size_type const & numMask)
    {
        dst.deactivate();
        DataVector::size_type j = 0;
        for (DataVector::size_type i = 0; i < numSrc; i++) {
            if (i < numMask) { if (!mask.at(i)) continue; }
            if (j >= numDst) dst.push_back(src.at(i));
            else             dst.at(j) = src.at(i);
            j++;
        }
    }
};


struct StatisticsTrackerOptions
{
    StatisticsTrackerOptions() { doMax=doMean=doMin=doVar=false; }
    bool doMax, doMean, doMin, doVar;
};

struct CommandLineOptions
{
    CommandLineOptions()
    {
        delimiter = ",";
        fileInput = "";
        filterColumns = std::vector<bool>();
        numLinesToKeep = std::numeric_limits<int>::max();
        numLinesToSkip = 0;
        showFilteredData = false;
        showStatistics = false;
        verboseLevel = 0;
        useThreads = true;
        numThreads = 0;
    }
    std::string delimiter;
    std::string fileInput;
    std::vector<bool> filterColumns;
    unsigned int numLinesToKeep;
    unsigned int numLinesToSkip;
    bool showFilteredData;
    bool showStatistics;
    StatisticsTrackerOptions statisticsOptions;
    unsigned char verboseLevel;
    bool useThreads;
    unsigned int numThreads;   /* N: definido na execução (-p N) */
};


class CommandLineParser
{
public:
    CommandLineParser(int argc, char * argv[]) :
      _showUsage(false), _showVersion(false)
    { parse(argc, argv); }

    static void printUsage()
    {
        std::cout <<
            "clistats - versão PARALELA com THREADS (N configurável)\n"
            "Uso: clistats [-h] [-V] [-v N] [-i arq] [-s N] [-k N] [-d delim]\n"
            "              [-fc filtro] [-fd] [-ss] -p N [--no-threads]\n"
            "  -p N, --processes N   número de threads (1 <= N <= 64; 1 = sequencial)\n"
            "  -d C, --delimiter C   delimitador (1 caractere). Padrão ','\n"
            "  --no-threads          força execução sequencial (equivale a -p 1)\n";
    }
    static void printVersion() { std::cout << ApplicationProperties::version() << std::endl; }

    bool showUsage()   const noexcept { return _showUsage;   }
    bool showVersion() const noexcept { return _showVersion; }

private:
    void parse(int argc, char * argv[])
    {
        std::list<std::string> arguments(argv, argv + argc);
        arguments.pop_front();
        while (!arguments.empty()) {
            auto argument = arguments.begin();
            std::string flag(*argument);
            if (flag == "-h" || flag == "--help")    { _showUsage = true; return; }
            if (flag == "-V" || flag == "--version") { _showVersion = true; return; }
            if (flag == "-v" || flag == "--verbose") {
                unsigned int verbose = 0;
                std::string value = (++argument != arguments.end()) ? *argument : "";
                if (!StringParser::toValue<unsigned int>(value, verbose) ||
                    verbose > Logger::Level::DETAIL)
                    throw std::runtime_error("Nível de verbosidade inválido");
                options.verboseLevel = (unsigned char)verbose;
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "-i" || flag == "--input") {
                std::string value = (++argument != arguments.end()) ? *argument : "";
                std::ifstream f(value.c_str());
                if (!f.good()) throw std::runtime_error("Arquivo de entrada inválido");
                options.fileInput = value;
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "-d" || flag == "--delimiter") {
                std::string value = (++argument != arguments.end()) ? *argument : "";
                if (value.empty()) throw std::runtime_error("Delimitador inválido");
                if (value.size() != 1)
                    throw std::runtime_error("Delimitador deve ter exatamente 1 caractere");
                options.delimiter = value;
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "-s" || flag == "--skip") {
                int skip = 0;
                std::string value = (++argument != arguments.end()) ? *argument : "";
                if (!StringParser::toValue<int>(value, skip) || skip < 0)
                    throw std::runtime_error("Número inválido de linhas para pular");
                options.numLinesToSkip = static_cast<unsigned int>(skip);
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "-k" || flag == "--keep") {
                int keep = 0;
                std::string value = (++argument != arguments.end()) ? *argument : "";
                if (!StringParser::toValue<int>(value, keep) || keep <= 0)
                    throw std::runtime_error("Número inválido de linhas a manter");
                options.numLinesToKeep = static_cast<unsigned int>(keep);
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "-fc" || flag == "--filterColumn") {
                std::string value = (++argument != arguments.end()) ? *argument : "";
                StringSplitter parser(value, ",");
                std::vector<int> filters;
                for (std::string const & token : parser.tokens()) {
                    std::vector<int> range;
                    if (!StringSplitter::toIntegers(token, range))
                        throw std::runtime_error("Valor de coluna de filtro inválido = " + token);
                    for (int e : range) {
                        if (e <= 0)
                            throw std::runtime_error("Valor de coluna de filtro inválido = " + token);
                        filters.push_back(e - 1);
                    }
                }
                if (filters.empty()) throw std::runtime_error("Filtro vazio");
                int maxFilter = *std::max_element(filters.begin(), filters.end());
                options.filterColumns.assign(maxFilter + 1, false);
                for (int f : filters) options.filterColumns.at(f) = true;
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "-fd" || flag == "--filtered") {
                options.showFilteredData = true; arguments.pop_front();
            }
            else if (flag == "-ss" || flag == "--statistics") {
                options.showStatistics = true; arguments.pop_front();
            }
            else if (flag == "-p" || flag == "--processes") {
                int n = 0;
                std::string value = (++argument != arguments.end()) ? *argument : "";
                if (!StringParser::toValue<int>(value, n) || n < 1 || n > 64)
                    throw std::runtime_error("Número de threads inválido (use -p N, 1 <= N <= 64)");
                options.numThreads = static_cast<unsigned int>(n);
                arguments.pop_front(); arguments.pop_front();
            }
            else if (flag == "--no-threads") {
                options.useThreads = false; arguments.pop_front();
            }
            else throw std::runtime_error("Flag não reconhecida (" + flag + ")");
        }
        if (options.delimiter.size() != 1)
            throw std::runtime_error("Delimitador deve ter exatamente 1 caractere");
        if (options.numThreads == 1) options.useThreads = false;
        if (options.useThreads && options.numThreads == 0)
            throw std::runtime_error("Informe o número de threads com -p N (ou use --no-threads)");
        if (!options.showStatistics)
            options.showStatistics = !options.showFilteredData;
        options.statisticsOptions.doMax  = options.showStatistics;
        options.statisticsOptions.doMean = options.showStatistics;
        options.statisticsOptions.doMin  = options.showStatistics;
        options.statisticsOptions.doVar  = options.showStatistics;
    }

public:
    CommandLineOptions options;
private:
    bool _showUsage, _showVersion;
};


/* ------------------- NÚCLEO ESTATÍSTICO (Welford) --------------------- */

class StatisticsTracker
{
public:
    explicit StatisticsTracker(StatisticsTrackerOptions const & options) :
        _count(0), _maximum(0), _mean(0), _minimum(0), _name(""),
        _options(options), _variance(0) {}

    long getCount() const noexcept { return _count; }
    double getMinimum() const noexcept { return _minimum; }
    double getMean() const noexcept { return _mean; }
    double getMaximum() const noexcept { return _maximum; }
    std::string getName() const { return _name; }
    double getVariance() const noexcept { return _variance; }
    void setName(std::string const & name) { _name = name; }

    void update(double value)
    {
        _count++;
        if (_count == 1) {
            if (_options.doMin)  _minimum  = value;
            if (_options.doMax)  _maximum  = value;
            if (_options.doMean) _mean     = value;
            if (_options.doVar)  _variance = 0;
            return;
        }
        if (_options.doMin && value < _minimum) _minimum = value;
        if (_options.doMax && value > _maximum) _maximum = value;
        if (_options.doMean || _options.doVar) {
            double delta = value - _mean;
            if (_options.doMean) _mean += delta / static_cast<double>(_count);
            if (_options.doVar) {
                double count = static_cast<double>(_count);
                _variance = (count - 1) * _variance + delta * (value - _mean);
                _variance /= count;
            }
        }
    }

protected:
    long _count;
    double _maximum, _mean, _minimum;
    std::string _name;
    StatisticsTrackerOptions _options;
    double _variance;
};


class MultivariateTracker
{
public:
    MultivariateTracker(long size, StatisticsTrackerOptions const & options) :
        _options(options) { initialize(size, options); }

    long getCount(long index) const { return _statistics[index].getCount(); }
    long getDimension() const noexcept { return static_cast<long>(_statistics.size()); }
    double getMinimum(long index) const { return _statistics[index].getMinimum(); }
    double getMean(long index) const { return _statistics[index].getMean(); }
    double getMaximum(long index) const { return _statistics[index].getMaximum(); }
    std::string getName(long index) const { return _statistics[index].getName(); }
    double getVariance(long index) const { return _statistics[index].getVariance(); }
    void setName(long index, std::string const & name)
    { _statistics[index].setName(name); }

    bool update(DataVector const & data)
    {
        if (_statistics.size() != 0 && data.size() != _statistics.size()) return false;
        if (_statistics.size() == 0) {
            if (data.size() == 0) return false;
            initialize(static_cast<long>(data.size()), _options);
        }
        auto datum = data.begin();
        auto tracker = _statistics.begin();
        for ( ; tracker != _statistics.end(); ++tracker, ++datum)
            if (datum->active) tracker->update(datum->value);
        return true;
    }

private:
    void initialize(long size, StatisticsTrackerOptions const & options)
    {
        for (long i = 0; i < size; ++i)
            _statistics.push_back(StatisticsTracker(options));
    }

protected:
    StatisticsTrackerOptions _options;
    std::vector<StatisticsTracker> _statistics;
};


class StatisticsWriter
{
public:
    StatisticsWriter(std::string const & comment = "#") : _comment(comment) {}

    void writeData(std::ostream & stream, DataVector const & data,
                   std::string const & delimiter = " ")
    {
        for (auto it = data.begin(); it != data.end();) {
            if (!it->active) { ++it; continue; }
            stream << it->value;
            ++it;
            if (it != data.end()) stream << delimiter;
        }
        stream << '\n';
    }

    void writeStatistics(std::ostream & stream, MultivariateTracker & tracker)
    {
        const long numDim = tracker.getDimension();
        std::vector<int> width(6, 0);
        std::string titles[6] = {"Dimension","Count","Minimum","Mean","Maximum","Stdev"};
        for (int k = 0; k < 6; k++) width[k] = static_cast<int>(titles[k].size());
        for (long i = 0; i < numDim; i++) {
            std::string name  = tracker.getName(i);
            std::string count = StringParser::parseNumber<long>(tracker.getCount(i));
            std::string minv  = StringParser::parseNumber<double>(tracker.getMinimum(i));
            std::string mean  = StringParser::parseNumber<double>(tracker.getMean(i));
            std::string maxv  = StringParser::parseNumber<double>(tracker.getMaximum(i));
            std::string stdev = StringParser::parseNumber<double>(std::sqrt(tracker.getVariance(i)));
            width[0] = std::max<int>(width[0], static_cast<int>(name.size()));
            width[1] = std::max<int>(width[1], static_cast<int>(count.size()));
            width[2] = std::max<int>(width[2], static_cast<int>(minv.size()));
            width[3] = std::max<int>(width[3], static_cast<int>(mean.size()));
            width[4] = std::max<int>(width[4], static_cast<int>(maxv.size()));
            width[5] = std::max<int>(width[5], static_cast<int>(stdev.size()));
        }
        int totalWidth = 0;
        for (int & entry : width) { entry += 3; totalWidth += entry; }

        std::string header(totalWidth, '='), footer(totalWidth, '-');
        stream << _comment << header << std::endl;
        stream << _comment << std::setfill(' ') << std::right
               << std::setw(totalWidth/2) << "Statistics" << std::endl;
        stream << _comment << header << std::endl;
        stream << _comment;
        stream << std::right << std::setw(width[0]) << "Dimension";
        stream << std::right << std::setw(width[1]) << "Count";
        stream << std::right << std::setw(width[2]) << "Minimum";
        stream << std::right << std::setw(width[3]) << "Mean";
        stream << std::right << std::setw(width[4]) << "Maximum";
        stream << std::right << std::setw(width[5]) << "Stdev";
        stream << std::endl;
        stream << _comment << footer << std::endl;

        std::string indent(_comment.size(), ' ');
        for (long i = 0; i < numDim; i++) {
            stream << indent;
            stream << std::right << std::setw(width[0]) << tracker.getName(i);
            stream << std::right << std::setw(width[1]) << StringParser::parseNumber<long>(tracker.getCount(i));
            stream << std::right << std::setw(width[2]) << StringParser::parseNumber<double>(tracker.getMinimum(i));
            stream << std::right << std::setw(width[3]) << StringParser::parseNumber<double>(tracker.getMean(i));
            stream << std::right << std::setw(width[4]) << StringParser::parseNumber<double>(tracker.getMaximum(i));
            stream << std::right << std::setw(width[5]) << StringParser::parseNumber<double>(std::sqrt(tracker.getVariance(i)));
            stream << std::endl;
        }
        stream << std::endl;
    }

private:
    std::string _comment;
};


/* ---------------- MEMÓRIA COMPARTILHADA ENTRE THREADS ---------------- */

struct MemoriaCompartilhada
{
    MemoriaCompartilhada(long dimensoes,
                         StatisticsTrackerOptions const & opcoes,
                         int numThreads) :
        estatisticas(dimensoes, opcoes),
        totalLinhas(0),
        linhasPorThread(numThreads, 0) {}

    std::mutex mutex;
    MultivariateTracker estatisticas;
    long totalLinhas;
    std::vector<long> linhasPorThread;
};


/* ------------------------- APLICAÇÃO -------------------------------- */

class StatisticsApp
{
public:
    StatisticsApp(CommandLineOptions const & options) :
        _options(options), _outputStream(0),
        _tracker(MultivariateTracker(0, options.statisticsOptions))
    {
        Logger::logLevel = (Logger::Level::Type)_options.verboseLevel;
        _outputStream = &std::cout;
    }

    int run()
    {
        _usedThreads = false;
        _threadCount = 0;

        if (_options.useThreads && _options.numThreads >= 2) {
            /* Decide se dá para paralelizar:
             *  - precisa de arquivo (não stdin);
             *  - -fd exige ordem (fica sequencial);
             *  - -k é contagem global (fica sequencial). */
            bool parallel = !_options.fileInput.empty() &&
                            !_options.showFilteredData &&
                            _options.numLinesToKeep == static_cast<unsigned int>(std::numeric_limits<int>::max());
            if (parallel) {
                std::ifstream probe(_options.fileInput.c_str(), std::ios::in | std::ios::binary);
                if (probe.good()) {
                    probe.seekg(0, std::ios::end);
                    std::streamoff sz = probe.tellg();
                    if (sz > 0)
                        return runParallel(static_cast<unsigned long long>(sz));
                }
            }
        }
        return runSequential();
    }

    bool usedThreads() const noexcept { return _usedThreads; }
    unsigned int threadCount() const noexcept { return _threadCount; }

    bool display()
    {
        StatisticsWriter writer;
        if (_options.showStatistics)
            writer.writeStatistics(*_outputStream, _tracker);
        return true;
    }

private:
    /* -------------------- SEQUENCIAL (mesmo do so_sequencial) --------- */
    int runSequential()
    {
        int numDim = 0, numDimMask = 0;

        std::ifstream fileInput(_options.fileInput.c_str(), std::ios::in);
        std::istream * source = (fileInput.good() ? &fileInput : &std::cin);

        StatisticsWriter writer;
        StringSplitter parser;
        unsigned int counter = 0;
        DataVector data, dataMasked;

        while (true) {
            std::string line = readline(*source);
            if (!(*source).good()) break;
            if (line.size() == 0) continue;
            counter++;
            if (counter <= _options.numLinesToSkip) continue;
            if (counter > (_options.numLinesToSkip + _options.numLinesToKeep)) break;

            parser.initialize(line, _options.delimiter);
            StringSplitter::size_type numTokens = parser.size();

            if (numDim <= 0) {
                numDim = static_cast<int>(numTokens);
                data.resize(numDim);
                _options.filterColumns.resize(numDim, (_options.filterColumns.size() == 0));
                numDimMask = std::accumulate(_options.filterColumns.begin(),
                                             _options.filterColumns.end(), 0);
                dataMasked.resize(numDimMask);
            }

            for (StringSplitter::size_type i = 0; i < (StringSplitter::size_type)numDim; ++i) {
                if (numTokens <= i) break;
                data.at(i).active = parser.toValue<double>(i, data.at(i).value);
            }

            DataVector::copy(data, numDim, dataMasked, numDimMask,
                             _options.filterColumns, numDim);

            if (_options.showFilteredData)
                writer.writeData(*_outputStream, dataMasked);

            _tracker.update(dataMasked);
            data.deactivate();
        }

        for (long i = 0; i < numDimMask && i < _tracker.getDimension(); i++)
            _tracker.setName(i, StringParser::parseNumber<long>(i + 1));

        return 0;
    }

    /* -------------------- PARALELO COM N THREADS ---------------------- */
    int runParallel(unsigned long long fileSize)
    {
        const std::string path = _options.fileInput;
        const unsigned int numThreads = _options.numThreads;

        int numDim = 0, numDimMask = 0;
        unsigned long long dataStart = 0;
        bool found = false;

        /* Passo 1: descobre numDim e onde os dados começam (após -s). */
        {
            std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
            std::streambuf * buffer = file.rdbuf();
            unsigned long long pos = 0;
            unsigned int counter = 0;
            std::string line;
            StringSplitter splitter;

            while (true) {
                unsigned long long lineStart = pos;
                if (!readLineCounted(buffer, line, pos)) break;
                if (line.empty()) continue;
                counter++;
                if (counter <= _options.numLinesToSkip) continue;
                splitter.initialize(line, _options.delimiter);
                numDim = static_cast<int>(splitter.size());
                dataStart = lineStart;
                found = true;
                break;
            }
        }
        if (!found) return 0;

        _options.filterColumns.resize(numDim, (_options.filterColumns.size() == 0));
        numDimMask = std::accumulate(_options.filterColumns.begin(),
                                     _options.filterColumns.end(), 0);

        /* Passo 2: fatias em N pedaços + N threads. */
        const unsigned long long total = (fileSize > dataStart) ? (fileSize - dataStart) : 0;
        std::vector<unsigned long long> limites(numThreads + 1);
        limites[0] = dataStart;
        limites[numThreads] = fileSize;
        for (unsigned int i = 1; i < numThreads; ++i)
            limites[i] = dataStart + total * i / numThreads;

        MemoriaCompartilhada memoria(numDimMask, _options.statisticsOptions, numThreads);

        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        std::vector<std::exception_ptr> erros(numThreads);

        for (unsigned int t = 0; t < numThreads; ++t) {
            unsigned int tid = t + 1;
            unsigned long long begin = limites[t];
            unsigned long long end   = limites[t + 1];
            bool discardFirst = (t != 0);
            threads.emplace_back([&, tid, begin, end, discardFirst]() {
                try {
                    processChunk(path, begin, end, discardFirst,
                                 numDim, numDimMask, memoria, tid);
                } catch (...) {
                    erros[tid - 1] = std::current_exception();
                }
            });
        }

        for (auto & th : threads) th.join();
        for (auto & e : erros) if (e) std::rethrow_exception(e);

        _tracker = memoria.estatisticas;
        _usedThreads = true;
        _threadCount = numThreads;

        for (long i = 0; i < numDimMask && i < _tracker.getDimension(); i++)
            _tracker.setName(i, StringParser::parseNumber<long>(i + 1));

        for (unsigned int i = 0; i < numThreads; ++i) {
            logMessage(Logger::Level::INFO)
                << "THREAD " << (i + 1) << " escreveu "
                << memoria.linhasPorThread[i] << " linhas";
        }
        logMessage(Logger::Level::INFO) << "Total de linhas: " << memoria.totalLinhas;
        return 0;
    }

    /* -------------------- CÓDIGO DAS THREADS -------------------------- */
    void processChunk(std::string const & path,
                      unsigned long long begin, unsigned long long end,
                      bool discardFirst,
                      int numDim, int numDimMask,
                      MemoriaCompartilhada & memoria,
                      unsigned int threadId) const
    {
        long linhasDestaThread = 0;
        if (begin >= end) return;

        std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
        if (!file.good())
            throw std::runtime_error("Arquivo de entrada inválido");
        std::streambuf * buffer = file.rdbuf();

        unsigned long long pos = begin;
        std::string line;

        if (discardFirst && begin > 0) {
            pos = begin - 1;
            if (buffer->pubseekpos(static_cast<std::streamoff>(pos)) == std::streampos(-1))
                throw std::runtime_error("Falha ao posicionar");
            readLineCounted(buffer, line, pos);
        } else if (begin > 0) {
            if (buffer->pubseekpos(static_cast<std::streamoff>(begin)) == std::streampos(-1))
                throw std::runtime_error("Falha ao posicionar");
        }

        StringSplitter parser;
        DataVector data, dataMasked;
        data.resize(numDim);
        dataMasked.resize(numDimMask);

        while (pos < end && readLineCounted(buffer, line, pos)) {
            if (line.empty()) continue;
            linhasDestaThread++;

            parser.initialize(line, _options.delimiter);
            StringSplitter::size_type numTokens = parser.size();

            for (StringSplitter::size_type i = 0; i < (StringSplitter::size_type)numDim; ++i) {
                if (numTokens <= i) break;
                data.at(i).active = parser.toValue<double>(i, data.at(i).value);
            }

            DataVector::copy(data, numDim, dataMasked, numDimMask,
                             _options.filterColumns, numDim);
            data.deactivate();

            /* ============== SEÇÃO CRÍTICA ============== */
            {
                std::lock_guard<std::mutex> trava(memoria.mutex);
                memoria.estatisticas.update(dataMasked);
                memoria.totalLinhas++;
                memoria.linhasPorThread[threadId - 1]++;
            }
            /* ========================================== */
        }

        logMessage(Logger::Level::INFO) << "THREAD " << threadId
            << " [" << begin << "," << end << ") processou "
            << linhasDestaThread << " linhas";
    }

    /* -------------------- readline helpers ---------------------------- */
    static bool readLineCounted(std::streambuf * buffer,
                                std::string & line,
                                unsigned long long & pos)
    {
        line.clear();
        while (true) {
            int character = buffer->sbumpc();
            if (character == EOF) return !line.empty();
            ++pos;
            if (character == '\n') return true;
            if (character == '\r') {
                if (buffer->sgetc() == '\n') { buffer->sbumpc(); ++pos; }
                return true;
            }
            line += static_cast<char>(character);
        }
    }

    static std::string readline(std::istream & stream)
    {
        std::string line;
        std::istream::sentry guard(stream, true);
        std::streambuf * buffer = stream.rdbuf();
        while (true) {
            int character = buffer->sbumpc();
            if (character == '\n') return line;
            if (character == '\r') {
                if (buffer->sgetc() == '\n') buffer->sbumpc();
                return line;
            }
            if (character == EOF) {
                if (line.empty()) stream.setstate(std::ios::eofbit);
                return line;
            }
            line += static_cast<char>(character);
        }
    }

private:
    CommandLineOptions _options;
    std::ostream * _outputStream;
    MultivariateTracker _tracker;
    bool _usedThreads = false;
    unsigned int _threadCount = 0;
};


/* ------------------------- DRIVER / MENU ----------------------------- */

namespace AppStatus {
    enum Status { SUCCESS = 0, FAILED_PARSING, FAILED_RUN, FAILED_DISPLAY };
}

std::string readTextOrDefault(std::string const & defaultValue)
{
    std::string input;
    std::getline(std::cin, input);
    return input.empty() ? defaultValue : input;
}

unsigned int readWholeNumberOrDefault(unsigned int defaultValue)
{
    while (true) {
        std::string input = readTextOrDefault(StringParser::parseNumber<unsigned int>(defaultValue));
        try {
            int parsed = std::stoi(input);
            if (parsed < 0) { std::cout << "  >= 0.\n> "; continue; }
            return static_cast<unsigned int>(parsed);
        } catch (...) { std::cout << "  Número inválido.\n> "; }
    }
}

void interactiveMenu(CommandLineOptions & options)
{
    std::cout << "\n=== Menu Interativo (Enter = valor entre [ ]) ===\n";
    std::cout << "Arquivo de entrada (vazio = teclado): ";
    options.fileInput = readTextOrDefault("");
    std::cout << "Linhas de cabeçalho a pular [1]: ";
    options.numLinesToSkip = readWholeNumberOrDefault(1);
    std::cout << "Número de threads [4]: ";
    options.numThreads = readWholeNumberOrDefault(4);
    if (options.numThreads < 1) options.numThreads = 1;
    std::cout << "Colunas (ex: 1,2,3 ou 1:4) [1,2,3,4]: ";
    std::string columnsInput = readTextOrDefault("1,2,3,4");
    std::cout << "Delimitador [,]: ";
    std::string delim = readTextOrDefault(",");
    if (delim.size() != 1) delim = ",";
    options.delimiter = delim;

    std::vector<int> filters;
    for (std::string const & token : StringSplitter(columnsInput, ",").tokens()) {
        std::vector<int> range;
        if (StringSplitter::toIntegers(token, range))
            for (int e : range) if (e > 0) filters.push_back(e - 1);
    }
    if (!filters.empty()) {
        int maxFilter = *std::max_element(filters.begin(), filters.end());
        options.filterColumns.assign(maxFilter + 1, false);
        for (int f : filters) options.filterColumns.at(f) = true;
    }
    options.showStatistics = true;
    options.statisticsOptions.doMax = true;
    options.statisticsOptions.doMean = true;
    options.statisticsOptions.doMin = true;
    options.statisticsOptions.doVar = true;
    options.useThreads = (options.numThreads > 1);
    std::cout << "\n";
}

int pdrv(int argc, char * argv[])
{
    CommandLineOptions options;
    if (argc <= 1) {
        interactiveMenu(options);
    } else {
        try {
            CommandLineParser parser(argc, argv);
            if (parser.showUsage())   { CommandLineParser::printUsage();   return AppStatus::SUCCESS; }
            if (parser.showVersion()) { CommandLineParser::printVersion(); return AppStatus::SUCCESS; }
            options = parser.options;
        } catch (std::exception & ex) {
            logMessage(Logger::Level::FATAL) << ex.what();
            return AppStatus::FAILED_PARSING;
        }
    }

    StatisticsApp application(options);
    try {
        auto t0 = std::chrono::steady_clock::now();
        int status = application.run();
        if (status != 0) return AppStatus::FAILED_RUN;
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();

        std::ostringstream info;
        info << std::fixed << std::setprecision(4);
        if (application.usedThreads()) {
            info << "[RESUMO] Modo: THREADS | threads: "
                 << application.threadCount()
                 << " (PID " << getpid() << ")";
        } else {
            info << "[RESUMO] Modo: SEQUENCIAL | threads: 1 (PID " << getpid() << ")";
            if (options.numThreads > 1)
                info << " — pedido -p " << options.numThreads
                     << ", mas o modo threads NAO foi usado (fallback)";
        }
        info << " | tempo de processamento: " << secs << " s";
        std::cerr << info.str() << std::endl;
    } catch (std::exception & ex) {
        logMessage(Logger::Level::FATAL) << ex.what();
        return AppStatus::FAILED_RUN;
    }

    try {
        if (!application.display()) return AppStatus::FAILED_DISPLAY;
    } catch (std::exception & ex) {
        logMessage(Logger::Level::FATAL) << ex.what();
        return AppStatus::FAILED_DISPLAY;
    }
    return AppStatus::SUCCESS;
}

#ifndef _CLISTATS_TESTING
int main(int argc, char * argv[]) { return pdrv(argc, argv); }
#endif