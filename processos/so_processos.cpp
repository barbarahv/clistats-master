/* ============================================================================
 * ARQUIVO: so_processos.cpp
 *
 * VERSÃO PARALELA COM PROCESSOS (fork + mmap) — NÚMERO DE PROCESSOS DEFINIDO
 * NA EXECUÇÃO (-p N / --processes N). Nenhum 2, 4 ou 8 fixo no código.
 *
 * Segue a MESMA BASE de so_sequencial.cpp (mesmas classes, mesmo parser,
 * mesmo Welford, mesma saída). O que muda é só a divisão do arquivo em N
 * fatias processadas por N processos (pai + N-1 filhos).
 *
 *   - O arquivo é dividido em N fatias (cortes sempre no próximo '\n').
 *   - Filhos 1..N-1 processam as fatias 0..N-2 e gravam o resultado em
 *     memória compartilhada (mmap anônimo).
 *   - O PAI processa a última fatia enquanto os filhos trabalham, depois
 *     espera todos (waitpid) e consolida (merge de Welford).
 *   - -p 1 ou --no-fork: execução sequencial (mesma coisa que so_sequencial).
 *
 * Exemplo:  ./so_processos -i dados.csv -s 1 -fc 1,2,3,4 -p 4
 * ============================================================================ */

/* Cabeçalhos padrão da biblioteca C++ */
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <atomic>
#include <chrono>

/* POSIX — fork, waitpid, mmap */
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <cstdlib>


struct ApplicationProperties
{
    static std::string version()
    {
        return std::string(ApplicationProperties::VERSION_MAJOR) + "." +
            ApplicationProperties::VERSION_MINOR + "." +
            ApplicationProperties::VERSION_PATCH;
    }
    static constexpr const char * NAME          = "clistats";
    static constexpr const char * AUTHOR        = "dpmcmlxxvi@gmail.com";
    static constexpr const char * VERSION_MAJOR = "1";
    static constexpr const char * VERSION_MINOR = "1";
    static constexpr const char * VERSION_PATCH = "0";
};

struct Logger
{
    struct Level
    {
        enum Type { FATAL = 0, ERROR, WARNING, INFO, DEBUG, DETAIL };
    };

    static std::ostream & log(Logger::Level::Type level)
    {
        std::string label = (level==Logger::Level::FATAL  ? "FATAL":
                            (level==Logger::Level::ERROR  ? "ERROR":
                            (level==Logger::Level::WARNING? "WARNING":
                            (level==Logger::Level::INFO   ? "INFO":
                            (level==Logger::Level::DEBUG  ? "DEBUG":"DETAIL")))));

        std::ostream * os = (level <= Logger::Level::WARNING ? &std::cerr : &std::cout);
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
        _enabled(level <= Logger::logLevel),
        _stream(0)
    {
        if (this->_enabled)
        {
            this->_stream = &Logger::log(level);
        }
    }

    ~LogStream()
    {
        if (this->_enabled)
        {
            *(this->_stream) << std::endl;
        }
    }

    template <class T>
    LogStream & operator<<(T const & value)
    {
        if (this->_enabled)
        {
            *(this->_stream) << value;
        }
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


class StringSplitter
{
public:
    typedef std::vector<std::string>::size_type size_type;

    StringSplitter() {}

    StringSplitter(std::string const & src, std::string const & delim)
    {
        this->initialize(src, delim);
    }

    std::string const & at(size_type index) const
    {
        return this->_tokens.at(index);
    }

    void initialize(std::string const & src, std::string const & delim)
    {
        std::vector<std::string> tokens;
        std::string::size_type startpos = 0;
        std::string::size_type endpos = 0;

        while (true)
        {
            endpos = src.find(delim, startpos);
            tokens.push_back(src.substr(startpos, endpos - startpos));
            if (endpos == std::string::npos) break;
            startpos = endpos + delim.size();
        }

        this->_tokens.swap(tokens);
    }

    size_type size() const
    {
        return this->_tokens.size();
    }

    std::vector<std::string> tokens() const
    {
        return this->_tokens;
    }

    template <class T>
    bool toValue(size_type index, T & dst) const
    {
        return StringParser::toValue<T>(this->at(index), dst);
    }

    static bool toIntegers(std::string const & src, std::vector<int> & dst,
                           std::string const & delim = ":")
    {
        int start = 0;
        int stop = 0;

        StringSplitter splitter(src, delim);

        if (splitter.size() == 1)
        {
            bool good = StringParser::toValue<int>(splitter.at(0), start);
            if (!good) return false;
            stop = start;
        }
        else if (splitter.size() == 2)
        {
            bool goodStart = StringParser::toValue<int>(splitter.at(0), start);
            if (!goodStart) return false;
            bool goodStop = StringParser::toValue<int>(splitter.at(1), stop);
            if (!goodStop) return false;
            if (stop < start) std::swap(start, stop);
        }
        else
        {
            return false;
        }

        for (int i = start; i < stop+1; i++)
        {
            dst.push_back(i);
        }

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
    {
        for (DataPoint & point : *this)
        {
            point.active = false;
        }
    }

    static void copy(DataVector const & src,
                     DataVector::size_type const & numSrc,
                     DataVector & dst,
                     DataVector::size_type const & numDst,
                     std::vector<bool> const & mask,
                     DataVector::size_type const & numMask)
    {
        dst.deactivate();

        DataVector::size_type j = 0;
        for (DataVector::size_type i = 0; i < numSrc; i++)
        {
            if (i < numMask)
            {
                if (!mask.at(i)) continue;
            }

            if (j >= numDst)
            {
                dst.push_back(src.at(i));
            }
            else
            {
                dst.at(j) = src.at(i);
            }
            j++;
        }
    }
};


struct StatisticsTrackerOptions
{
    StatisticsTrackerOptions()
    {
        this->doMax = false;
        this->doMean = false;
        this->doMin = false;
        this->doVar = false;
    }
    bool doMax;
    bool doMean;
    bool doMin;
    bool doVar;
};


struct CommandLineOptions
{
    CommandLineOptions()
    {
        this->delimiter = "";
        this->fileInput = "";
        this->filterColumns = std::vector<bool>();
        this->numLinesToKeep = std::numeric_limits<int>::max();
        this->numLinesToSkip = 0;
        this->showFilteredData = false;
        this->showStatistics = false;
        this->verboseLevel = 0;
        this->useFork = true;
        this->numProcesses = 0;
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
    bool useFork;
    unsigned int numProcesses;   /* N: definido na execução (-p N) */
};


class CommandLineParser
{
public:
    CommandLineParser(int argc, char * argv[]) :
      _showUsage(false),
      _showVersion(false)
    {
        this->parse(argc, argv);
    }

    static void printUsage()
    {
        std::cout << std::endl;
        std::cout << "NAME" << std::endl;
        std::cout << std::endl;
        std::cout << "    clistats - Command line statistics tool (parallel, N processes)" << std::endl;
        std::cout << std::endl;
        std::cout << "SYNOPSIS" << std::endl;
        std::cout << std::endl;
        std::cout << "    clistats [-h] [-v <level>] [-V] [-i <file>] [-s <rows>] [-k <rows>]" << std::endl;
        std::cout << "             [-d <char>] [-fc <filter>] [-fd] [-ss] -p <N> [--no-fork]" << std::endl;
        std::cout << std::endl;
        std::cout << "DESCRIPTION" << std::endl;
        std::cout << std::endl;
        std::cout << "    Parallel version: divides the file into N slices, forks N-1 children" << std::endl;
        std::cout << "    to process them, the parent processes the last slice, then merges" << std::endl;
        std::cout << "    the partial Welford statistics via shared memory." << std::endl;
        std::cout << std::endl;
        std::cout << "OPTIONS" << std::endl;
        std::cout << std::endl;
        std::cout << "        -p <N>, --processes <N>" << std::endl;
        std::cout << "            Number of processes (1 <= N <= 256). 1 = sequential." << std::endl;
        std::cout << std::endl;
        std::cout << "        --no-fork" << std::endl;
        std::cout << "            Force sequential execution (same as -p 1)." << std::endl;
        std::cout << std::endl;
        std::cout << "        -d <char>, --delimiter <char>" << std::endl;
        std::cout << "            Field delimiter (exactly 1 character). Default = ','." << std::endl;
        std::cout << std::endl;
        std::cout << "        (all other flags are the same as so_sequencial)" << std::endl;
        std::cout << std::endl;
    }

    static void printVersion()
    {
        std::cout << ApplicationProperties::version() << std::endl;
    }

    bool showUsage() const noexcept { return this->_showUsage; }
    bool showVersion() const noexcept { return this->_showVersion; }

private:
    void parse(int argc, char * argv[])
    {
        std::list<std::string> arguments(argv, argv + argc);
        arguments.pop_front();

        while (!arguments.empty())
        {
            auto argument = arguments.begin();
            std::string flag(*argument);

            if (flag == "-h" || flag == "--help")
            {
                this->_showUsage = true;
                return;
            }
            else if (flag == "-V" || flag == "--version")
            {
                this->_showVersion = true;
                return;
            }
            else if (flag == "-v" || flag == "--verbose")
            {
                unsigned int verbose = 0;
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;
                bool isInt = StringParser::toValue<unsigned int>(value, verbose);
                if (!(isInt && verbose <= Logger::Level::DETAIL))
                    throw std::runtime_error("Nível de verbosidade inválido");
                this->options.verboseLevel = (unsigned char)verbose;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-i" || flag == "--input")
            {
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;
                std::ifstream f(value.c_str());
                if (!f.good())
                    throw std::runtime_error("Arquivo de entrada inválido");
                this->options.fileInput = value;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-d" || flag == "--delimiter")
            {
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;
                if (value.empty())
                    throw std::runtime_error("Delimitador inválido");
                if (value.size() != 1)
                    throw std::runtime_error("Delimitador deve ter exatamente 1 caractere");
                this->options.delimiter = value;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-s" || flag == "--skip")
            {
                int skip = 0;
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;
                bool isInt = StringParser::toValue<int>(value, skip);
                if (!(isInt && skip >= 0))
                    throw std::runtime_error("Número inválido de linhas para pular");
                this->options.numLinesToSkip = skip;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-k" || flag == "--keep")
            {
                int keep = 0;
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;
                bool isInt = StringParser::toValue<int>(value, keep);
                if (!(isInt && keep > 0))
                    throw std::runtime_error("Número inválido de linhas a manter");
                this->options.numLinesToKeep = keep;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-fc" || flag == "--filterColumn")
            {
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;

                StringSplitter parser(value, ",");
                std::vector<std::string> values = parser.tokens();

                std::vector<int> filters;
                for (unsigned int j = 0; j < values.size(); j++)
                {
                    std::vector<int> range;
                    bool isRangeValid = StringSplitter::toIntegers(values.at(j), range);
                    if (!isRangeValid)
                        throw std::runtime_error("Valor de coluna de filtro inválido = " + values.at(j));

                    for (int entry : range)
                    {
                        if (entry <= 0)
                            throw std::runtime_error("Valor de coluna de filtro inválido = " + values.at(j));
                        filters.push_back(entry-1);
                    }
                }

                int maxFilter = *std::max_element(filters.begin(), filters.end());
                this->options.filterColumns.resize(maxFilter+1, false);
                for (unsigned int j = 0; j < filters.size(); j++)
                {
                    this->options.filterColumns.at(filters.at(j)) = true;
                }
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-fd" || flag == "--filtered")
            {
                this->options.showFilteredData = true;
                arguments.pop_front();
            }
            else if (flag == "-ss" || flag == "--statistics")
            {
                this->options.showStatistics = true;
                arguments.pop_front();
            }
            else if (flag == "-p" || flag == "--processes")
            {
                int n = 0;
                std::string value = "";
                if ((++argument) != arguments.end())
                    value = *argument;
                bool isInt = StringParser::toValue<int>(value, n);
                if (!(isInt && n >= 1 && n <= 256))
                    throw std::runtime_error("Número de processos inválido (use -p N, 1 <= N <= 256)");
                this->options.numProcesses = static_cast<unsigned int>(n);
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "--no-fork")
            {
                this->options.useFork = false;
                arguments.pop_front();
            }
            else
            {
                throw std::runtime_error("Flag não reconhecida (" + flag + "). Execute \"clistats --help\" para uso.");
            }
        }

        if (this->options.delimiter.empty())
        {
            this->options.delimiter = ",";
        }
        if (this->options.delimiter.size() != 1)
            throw std::runtime_error("Delimitador deve ter exatamente 1 caractere");

        if (this->options.numProcesses == 1)
            this->options.useFork = false;

        if (this->options.useFork && this->options.numProcesses == 0)
            throw std::runtime_error("Informe o número de processos com -p N (ou use --no-fork)");

        if (!this->options.showStatistics)
        {
            this->options.showStatistics = !(this->options.showFilteredData);
        }

        this->options.statisticsOptions.doMax = this->options.showStatistics;
        this->options.statisticsOptions.doMean = this->options.showStatistics;
        this->options.statisticsOptions.doMin = this->options.showStatistics;
        this->options.statisticsOptions.doVar = this->options.showStatistics;
    }

public:
    CommandLineOptions options;

private:
    bool _showUsage;
    bool _showVersion;
};


class StatisticsTracker
{
public:
    explicit StatisticsTracker(StatisticsTrackerOptions const & options) :
        _count(0),
        _maximum(0),
        _mean(0),
        _minimum(0),
        _name(""),
        _options(options),
        _variance(0)
    {}

    /* Reconstrói a partir do slot compartilhado (count, min, mean, max, var). */
    StatisticsTracker(StatisticsTrackerOptions const & options,
                      long count, double minimum, double mean,
                      double maximum, double variance) :
        _count(count),
        _maximum(maximum),
        _mean(mean),
        _minimum(minimum),
        _name(""),
        _options(options),
        _variance(variance)
    {}

    long getCount() const noexcept { return this->_count; }
    double getMinimum() const noexcept { return this->_minimum; }
    double getMean() const noexcept { return this->_mean; }
    double getMaximum() const noexcept { return this->_maximum; }
    std::string getName() const { return this->_name; }
    double getVariance() const noexcept { return this->_variance; }
    void setName(std::string const & name) { this->_name = name; }

    void update(double const value)
    {
        this->_count++;

        if (this->_count == 1)
        {
            if (this->_options.doMin)  this->_minimum = value;
            if (this->_options.doMax)  this->_maximum = value;
            if (this->_options.doMean) this->_mean = value;
            if (this->_options.doVar)  this->_variance = 0;
            return;
        }

        if (this->_options.doMin && value < this->_minimum) this->_minimum = value;
        if (this->_options.doMax && value > this->_maximum) this->_maximum = value;

        if (this->_options.doMean || this->_options.doVar)
        {
            double delta = value - this->_mean;

            if (this->_options.doMean)
            {
                this->_mean += delta / static_cast<double>(this->_count);
            }

            if (this->_options.doVar)
            {
                double count = static_cast<double>(this->_count);
                this->_variance = (count-1) * this->_variance + delta * (value - this->_mean);
                this->_variance /= count;
            }
        }
    }

    /* Merge de Welford com variância POPULACIONAL (consistente com update).
     *   mean = (nA*meanA + nB*meanB) / n
     *   var  = (nA*varA + nB*varB + nA*nB/n * (meanA-meanB)^2) / n
     */
    void merge(StatisticsTracker const & other)
    {
        if (other._count == 0) return;
        if (this->_count == 0) { *this = other; return; }

        long nA = this->_count;
        long nB = other._count;
        long n  = nA + nB;

        if (this->_options.doMin) this->_minimum = std::min(this->_minimum, other._minimum);
        if (this->_options.doMax) this->_maximum = std::max(this->_maximum, other._maximum);

        if (this->_options.doMean || this->_options.doVar)
        {
            double delta = other._mean - this->_mean;
            double meanNew = this->_mean + delta * static_cast<double>(nB) / static_cast<double>(n);

            if (this->_options.doVar)
            {
                double nA_d = static_cast<double>(nA);
                double nB_d = static_cast<double>(nB);
                double n_d  = static_cast<double>(n);
                this->_variance = (nA_d * this->_variance + nB_d * other._variance
                                   + nA_d * nB_d / n_d * delta * delta) / n_d;
            }
            if (this->_options.doMean) this->_mean = meanNew;
        }

        this->_count = n;
    }

protected:
    long _count;
    double _maximum;
    double _mean;
    double _minimum;
    std::string _name;
    StatisticsTrackerOptions _options;
    double _variance;
};


class MultivariateTracker
{
public:
    MultivariateTracker(long size, StatisticsTrackerOptions const & options) :
        _options(options)
    {
        this->initialize(size, options);
    }

    long getCount(long const index) const { return this->_statistics[index].getCount(); }
    long getDimension() const noexcept { return static_cast<long>(this->_statistics.size()); }
    double getMinimum(long const index) const { return this->_statistics[index].getMinimum(); }
    double getMean(long const index) const { return this->_statistics[index].getMean(); }
    double getMaximum(long const index) const { return this->_statistics[index].getMaximum(); }
    std::string getName(long const index) const { return this->_statistics[index].getName(); }
    double getVariance(long const index) const { return this->_statistics[index].getVariance(); }
    void setName(long const index, std::string const & name)
    { this->_statistics[index].setName(name); }

    bool update(DataVector const & data)
    {
        if (this->_statistics.size() != 0 && data.size() != this->_statistics.size())
            return false;

        if (this->_statistics.size() == 0)
        {
            if (data.size() == 0) return false;
            this->initialize(static_cast<long>(data.size()), this->_options);
        }

        auto datum = data.begin();
        auto tracker = this->_statistics.begin();
        for ( ; tracker != this->_statistics.end(); ++tracker, ++datum)
        {
            if (datum->active) tracker->update(datum->value);
        }
        return true;
    }

    void setStatistics(std::vector<StatisticsTracker> const & stats)
    {
        this->_statistics = stats;
    }

    bool merge(MultivariateTracker const & other)
    {
        if (other._statistics.empty()) return true;
        if (this->_statistics.empty()) { this->_statistics = other._statistics; return true; }
        if (this->_statistics.size() != other._statistics.size()) return false;

        for (size_t i = 0; i < this->_statistics.size(); i++)
        {
            this->_statistics[i].merge(other._statistics[i]);
        }
        return true;
    }

private:
    void initialize(long size, StatisticsTrackerOptions const & options)
    {
        for (long i = 0 ; i < size; ++i)
        {
            this->_statistics.push_back(StatisticsTracker(options));
        }
    }

protected:
    StatisticsTrackerOptions _options;
    std::vector<StatisticsTracker> _statistics;
};


class StatisticsWriter
{
public:
    StatisticsWriter(std::string const & comment = "#") :
        _comment(comment)
    {}

    void writeData(std::ostream & stream, DataVector const & data,
                   std::string const & delimiter = " ")
    {
        for (auto it = data.begin(); it != data.end();)
        {
            if (!it->active) { ++it; continue; }
            stream << it->value;
            ++it;
            if (it != data.end()) stream << delimiter;
        }
        stream << std::endl;
    }

    void writeStatistics(std::ostream & stream, MultivariateTracker & tracker)
    {
        const long numDim = tracker.getDimension();

        std::vector<int> width(6,0);
        std::string titles[6] = {"Dimension","Count","Minimum","Mean","Maximum","Stdev"};
        for (int k = 0; k < 6; k++)
            width[k] = static_cast<int>(titles[k].size());

        for (long i = 0; i < numDim; i++)
        {
            std::string name = tracker.getName(i);
            width[0] = std::max<int>(width[0], static_cast<int>(name.size()));
            std::string count = StringParser::parseNumber<long>(tracker.getCount(i));
            width[1] = std::max<int>(width[1], static_cast<int>(count.size()));
            std::string minv = StringParser::parseNumber<double>(tracker.getMinimum(i));
            width[2] = std::max<int>(width[2], static_cast<int>(minv.size()));
            std::string mean = StringParser::parseNumber<double>(tracker.getMean(i));
            width[3] = std::max<int>(width[3], static_cast<int>(mean.size()));
            std::string maxv = StringParser::parseNumber<double>(tracker.getMaximum(i));
            width[4] = std::max<int>(width[4], static_cast<int>(maxv.size()));
            std::string stdev = StringParser::parseNumber<double>(sqrt(tracker.getVariance(i)));
            width[5] = std::max<int>(width[5], static_cast<int>(stdev.size()));
        }

        int totalWidth = 0;
        for (int & entry : width) { entry += 3; totalWidth += entry; }

        std::string header(totalWidth,'=');
        std::string footer(totalWidth,'-');
        stream << this->_comment << header << std::endl;
        stream << this->_comment << std::setfill (' ') << std::setw (totalWidth/2) << "Statistics" << std::endl;
        stream << this->_comment << header << std::endl;
        stream << this->_comment;
        stream << std::right << std::setw(width[0]) << "Dimension";
        stream << std::right << std::setw(width[1]) << "Count";
        stream << std::right << std::setw(width[2]) << "Minimum";
        stream << std::right << std::setw(width[3]) << "Mean";
        stream << std::right << std::setw(width[4]) << "Maximum";
        stream << std::right << std::setw(width[5]) << "Stdev";
        stream << std::endl;
        stream << this->_comment << footer << std::endl;

        std::string indent(this->_comment.size(),' ');
        for (long i = 0; i < numDim; i++)
        {
            stream << indent;
            stream << std::right << std::setw(width[0]) << tracker.getName(i);
            stream << std::right << std::setw(width[1]) << StringParser::parseNumber<long>(tracker.getCount(i));
            stream << std::right << std::setw(width[2]) << StringParser::parseNumber<double>(tracker.getMinimum(i));
            stream << std::right << std::setw(width[3]) << StringParser::parseNumber<double>(tracker.getMean(i));
            stream << std::right << std::setw(width[4]) << StringParser::parseNumber<double>(tracker.getMaximum(i));
            stream << std::right << std::setw(width[5]) << StringParser::parseNumber<double>(sqrt(tracker.getVariance(i)));
            stream << std::endl;
        }
        stream << std::endl;
    }

private:
    std::string _comment;
};


class StatisticsApp
{
public:
    StatisticsApp(CommandLineOptions const & options) :
        _options(options),
        _outputStream(0),
        _tracker(MultivariateTracker(0, options.statisticsOptions))
    {
        Logger::logLevel = (Logger::Level::Type)this->_options.verboseLevel;
        this->_outputStream = &std::cout;
    }

    int run()
    {
        _forkedOk = false;
        _childPids.clear();

        if (this->_options.useFork && this->_options.numProcesses >= 2)
            return this->runForked();

        return this->runSequential();
    }

    bool usedFork() const noexcept { return _forkedOk; }
    std::vector<pid_t> const & childPids() const noexcept { return _childPids; }

    bool display()
    {
        StatisticsWriter writer;
        if (this->_options.showStatistics)
            writer.writeStatistics(*this->_outputStream, this->_tracker);
        return true;
    }

private:

    /* ---------------------------------------------------------------------
     * Estruturas de comunicação entre processos via mmap compartilhado.
     * ------------------------------------------------------------------- */
    struct SharedStatsUnit {
        double  min;
        double  mean;
        double  max;
        double  variance;
        int64_t count;
    };

    static constexpr size_t MAX_COLUMNS = 1024;

    struct SharedProcessSlot {
        uint32_t numDimensions;
        SharedStatsUnit stats[MAX_COLUMNS];
    };

    /* ---------------------------------------------------------------------
     * Processa uma fatia usando o MESMO parser do sequencial, mas com
     * uma delimitação de quantas linhas NÃO VAZIAS processar (para -k).
     * Recebe um sub-stream criado a partir do range de bytes.
     * ------------------------------------------------------------------- */
    unsigned int processByteRange(const char * buffer,
                                  size_t startByte, size_t endByte,
                                  MultivariateTracker & tracker,
                                  unsigned int maxLinesToKeep)
    {
        std::string slice(buffer + startByte, endByte - startByte);
        std::istringstream stream(slice);

        StringSplitter parser;
        unsigned int counter = 0;

        int numDim = 0;
        int numDimMask = 0;

        DataVector data;
        DataVector dataMasked;

        /* Cópia local das opções para não mexer em _options (cada filho
         * herda uma cópia própria no fork, mas o pai não deve ter
         * _options.filterColumns modificado pela última fatia). */
        std::vector<bool> filterColumns = this->_options.filterColumns;
        std::string delimiter = this->_options.delimiter;

        while (true)
        {
            std::string line = StatisticsApp::readline(stream);
            if (!stream.good()) break;
            if (line.size() == 0) continue;

            counter++;
            if (counter > maxLinesToKeep) break;

            parser.initialize(line, delimiter);
            StringSplitter::size_type numTokens = parser.size();

            if (numDim == 0)
            {
                numDim = static_cast<int>(numTokens);
                data.resize(numDim);
                filterColumns.resize(numDim, (filterColumns.size() == 0));
                numDimMask = std::accumulate(filterColumns.begin(), filterColumns.end(), 0);
                dataMasked.resize(numDimMask);
            }

            for (StringSplitter::size_type i = 0; i < (StringSplitter::size_type) numDim; ++i)
            {
                if (numTokens <= i) break;
                data.at(i).active = parser.toValue<double>(i, data.at(i).value);
            }

            DataVector::copy(data, numDim, dataMasked, numDimMask, filterColumns, numDim);
            tracker.update(dataMasked);
            data.deactivate();
        }

        return counter;
    }

    /* ---------------------------------------------------------------------
     * Versão sequencial (caminho de -p 1 / --no-fork). Idêntica ao
     * so_sequencial: lê do ifstream, processa linha a linha.
     * ------------------------------------------------------------------- */
    int runSequential()
    {
        int numDim = 0;
        int numDimMask = 0;

        std::ifstream fileInput(this->_options.fileInput.c_str(), std::ios::in);
        std::istream * source = (fileInput.good() ? &fileInput : &std::cin);

        logMessage(Logger::Level::INFO) << ApplicationProperties::NAME << " está iniciando";

        StatisticsWriter writer;
        StringSplitter parser;
        unsigned int counter = 0;

        DataVector data;
        DataVector dataMasked;

        while (true)
        {
            std::string line = StatisticsApp::readline(*source);
            if (!(*source).good()) break;
            if (line.size() == 0) continue;

            counter++;
            if (counter <= this->_options.numLinesToSkip) continue;
            if (counter > (this->_options.numLinesToSkip + this->_options.numLinesToKeep)) break;

            parser.initialize(line, this->_options.delimiter);
            StringSplitter::size_type numTokens = parser.size();

            bool isDimSet = numDim > 0;
            if (!isDimSet)
            {
                numDim = static_cast<int>(numTokens);
                data.resize(numDim);
                this->_options.filterColumns.resize(numDim, (this->_options.filterColumns.size() == 0));
                numDimMask = std::accumulate(this->_options.filterColumns.begin(),
                                             this->_options.filterColumns.end(), 0);
                dataMasked.resize(numDimMask);
            }

            for (StringSplitter::size_type i = 0; i < (StringSplitter::size_type) numDim; ++i)
            {
                if (numTokens <= i) break;
                data.at(i).active = parser.toValue<double>(i, data.at(i).value);
            }

            DataVector::copy(data, numDim, dataMasked, numDimMask,
                             this->_options.filterColumns, numDim);

            if (this->_options.showFilteredData)
                writer.writeData(*this->_outputStream, dataMasked);

            this->_tracker.update(dataMasked);
            data.deactivate();
        }

        for (long i = 0; i < numDimMask && i < this->_tracker.getDimension(); i++)
            this->_tracker.setName(i, StringParser::parseNumber<long>(i+1));

        logMessage(Logger::Level::INFO) << ApplicationProperties::NAME << " está completo";
        return 0;
    }

    /* ---------------------------------------------------------------------
     * Versão paralela: N fatias, pai + N-1 filhos.
     * ------------------------------------------------------------------- */
    int runForked()
    {
        if (this->_options.fileInput.empty())
        {
            logMessage(Logger::Level::FATAL) << "Arquivo de entrada não informado";
            return -1;
        }

        const size_t numSlices   = this->_options.numProcesses;
        const size_t numChildren = numSlices - 1;
        if (numSlices < 2) return -1;

        /* Lê o arquivo inteiro para um buffer. Filhos herdam via fork
         * (copy-on-write: o buffer é read-only depois da leitura). */
        std::ifstream in(this->_options.fileInput.c_str(), std::ios::in | std::ios::binary);
        if (!in.good())
        {
            logMessage(Logger::Level::FATAL) << "Arquivo de entrada inválido";
            return -1;
        }
        in.seekg(0, std::ios::end);
        std::streamoff fileSizeOff = in.tellg();
        in.seekg(0, std::ios::beg);
        if (fileSizeOff <= 0)
        {
            logMessage(Logger::Level::FATAL) << "Arquivo vazio";
            return -1;
        }
        std::vector<char> fileData(static_cast<size_t>(fileSizeOff));
        in.read(fileData.data(), fileSizeOff);
        in.close();
        if (!in)
        {
            logMessage(Logger::Level::FATAL) << "Falha lendo o arquivo";
            return -1;
        }

        const char * buffer = fileData.data();
        const size_t fileSize = fileData.size();
        const char delimChar = this->_options.delimiter[0];

        /* Utilitário: verifica se uma linha [p, e) tem algum caractere
         * que não seja delimitador, CR, espaço ou TAB. */
        auto isNonEmptyLine = [delimChar](const char* p, const char* e) -> bool {
            for (const char* q = p; q < e; ++q)
            {
                char c = *q;
                if (c != '\n' && c != '\r' && c != delimChar && c != ' ' && c != '\t')
                    return true;
            }
            return false;
        };

        /* ---- pula -s linhas NÃO VAZIAS ---- */
        size_t dataStartByte = 0;
        unsigned int skipped = 0;
        while (dataStartByte < fileSize && skipped < this->_options.numLinesToSkip)
        {
            const char* nl = static_cast<const char*>(
                std::memchr(buffer + dataStartByte, '\n', fileSize - dataStartByte));
            const char* lineEnd = nl ? nl : buffer + fileSize;
            if (isNonEmptyLine(buffer + dataStartByte, lineEnd)) ++skipped;
            dataStartByte = nl ? static_cast<size_t>(nl - buffer) + 1 : fileSize;
        }
        if (dataStartByte >= fileSize)
        {
            logMessage(Logger::Level::FATAL) << "Nenhuma linha sobrou após -s";
            return -1;
        }

        /* ---- divisão em N fatias, corte no próximo '\n' ---- */
        const size_t remainingBytes = fileSize - dataStartByte;
        std::vector<size_t> offsets(numSlices + 1);
        offsets[0] = dataStartByte;
        offsets[numSlices] = fileSize;
        for (size_t idx = 1; idx < numSlices; ++idx)
        {
            size_t target = dataStartByte + (remainingBytes * idx) / numSlices;
            const char* nl = static_cast<const char*>(
                std::memchr(buffer + target, '\n', fileSize - target));
            size_t off = nl ? static_cast<size_t>(nl - buffer) + 1 : fileSize;
            offsets[idx] = std::max(off, offsets[idx - 1]);
        }

        {
            LogStream ls = logMessage(Logger::Level::INFO);
            ls << "Divisão em " << numSlices << " fatias:";
            for (size_t i = 0; i < numSlices; ++i)
                ls << " [" << offsets[i] << "," << offsets[i + 1] << ")";
        }

        /* ---- orçamento de -k por fatia (só calcula se -k foi usado) ---- */
        const bool hasKeep = (this->_options.numLinesToKeep != (unsigned int)std::numeric_limits<int>::max());
        std::vector<unsigned int> keepPerSlice(numSlices,
            static_cast<unsigned int>(std::numeric_limits<int>::max()));
        if (hasKeep)
        {
            std::vector<unsigned int> linesPerSlice(numSlices, 0);
            for (size_t i = 0; i < numSlices; ++i)
            {
                const char* p = buffer + offsets[i];
                const char* e = buffer + offsets[i + 1];
                unsigned int c = 0;
                while (p < e)
                {
                    const char* nl = static_cast<const char*>(std::memchr(p, '\n', e - p));
                    const char* lineEnd = nl ? nl : e;
                    if (isNonEmptyLine(p, lineEnd)) ++c;
                    p = nl ? nl + 1 : e;
                }
                linesPerSlice[i] = c;
            }

            unsigned int budget = this->_options.numLinesToKeep;
            for (size_t i = 0; i < numSlices; ++i)
            {
                unsigned int k = std::min<unsigned int>(budget, linesPerSlice[i]);
                keepPerSlice[i] = k;
                budget -= k;
                if (budget == 0)
                {
                    for (size_t j = i + 1; j < numSlices; ++j) keepPerSlice[j] = 0;
                    break;
                }
            }
        }

        /* ---- memória compartilhada: um slot por filho ---- */
        const size_t shmSize = numChildren * sizeof(SharedProcessSlot);
        SharedProcessSlot * slots = static_cast<SharedProcessSlot *>(
            mmap(NULL, shmSize, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        if (slots == MAP_FAILED)
        {
            logMessage(Logger::Level::FATAL) << "mmap (shared) falhou";
            return -1;
        }
        std::memset(slots, 0, shmSize);

        std::vector<bool> sliceHasChild(numSlices, false);
        std::vector<pid_t> started;
        started.reserve(numChildren);

        /* ---- cria N-1 filhos ---- */
        for (size_t i = 0; i < numChildren; ++i)
        {
            pid_t pid = fork();
            if (pid < 0)
            {
                logMessage(Logger::Level::WARNING)
                    << "fork falhou para fatia " << i << " (" << std::strerror(errno)
                    << "); o pai processará essa fatia";
                continue;
            }
            if (pid == 0)
            {
                /* ---- código do filho ---- */
                MultivariateTracker stats_filho(0, this->_options.statisticsOptions);
                this->processByteRange(buffer, offsets[i], offsets[i + 1],
                                       stats_filho, keepPerSlice[i]);

                uint32_t numDim = static_cast<uint32_t>(stats_filho.getDimension());
                if (numDim > MAX_COLUMNS) numDim = MAX_COLUMNS;
                slots[i].numDimensions = numDim;
                for (uint32_t d = 0; d < numDim; d++)
                {
                    slots[i].stats[d].min      = stats_filho.getMinimum(d);
                    slots[i].stats[d].mean     = stats_filho.getMean(d);
                    slots[i].stats[d].max      = stats_filho.getMaximum(d);
                    slots[i].stats[d].variance = stats_filho.getVariance(d);
                    slots[i].stats[d].count    = stats_filho.getCount(d);
                }
                std::atomic_thread_fence(std::memory_order_release);
                munmap(slots, shmSize);
                _exit(0);
            }
            sliceHasChild[i] = true;
            started.push_back(pid);
        }

        /* ---- pai processa a última fatia (e qualquer fatia sem filho) ---- */
        std::map<size_t, MultivariateTracker> paiPorFatia;
        auto runSliceOnParent = [&](size_t i) {
            MultivariateTracker t(0, this->_options.statisticsOptions);
            this->processByteRange(buffer, offsets[i], offsets[i + 1],
                                   t, keepPerSlice[i]);
            paiPorFatia.emplace(i, std::move(t));
        };

        for (size_t i = numChildren; i < numSlices; ++i)
            runSliceOnParent(i);
        for (size_t i = 0; i < numChildren; ++i)
            if (!sliceHasChild[i]) runSliceOnParent(i);

        /* ---- espera filhos ---- */
        bool childFailed = false;
        for (pid_t p : started)
        {
            int status = 0;
            waitpid(p, &status, 0);
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) childFailed = true;
        }
        if (childFailed)
        {
            munmap(slots, shmSize);
            logMessage(Logger::Level::FATAL) << "Um filho terminou com erro";
            return -1;
        }

        std::atomic_thread_fence(std::memory_order_acquire);

        /* ---- merge em ordem global de fatias ---- */
        MultivariateTracker finalTracker(0, this->_options.statisticsOptions);
        for (size_t i = 0; i < numSlices; ++i)
        {
            if (sliceHasChild[i])
            {
                uint32_t dims = slots[i].numDimensions;
                std::vector<StatisticsTracker> trackers;
                trackers.reserve(dims);
                for (uint32_t d = 0; d < dims; d++)
                {
                    trackers.push_back(StatisticsTracker(
                        this->_options.statisticsOptions,
                        static_cast<long>(slots[i].stats[d].count),
                        slots[i].stats[d].min,
                        slots[i].stats[d].mean,
                        slots[i].stats[d].max,
                        slots[i].stats[d].variance));
                }
                MultivariateTracker procStats(0, this->_options.statisticsOptions);
                procStats.setStatistics(trackers);
                if (!finalTracker.merge(procStats))
                {
                    munmap(slots, shmSize);
                    logMessage(Logger::Level::FATAL)
                        << "Merge falhou (dimensões divergentes entre fatias)";
                    return -1;
                }
            }
            else
            {
                if (!finalTracker.merge(paiPorFatia.at(i)))
                {
                    munmap(slots, shmSize);
                    logMessage(Logger::Level::FATAL)
                        << "Merge falhou (dimensões divergentes entre fatias)";
                    return -1;
                }
            }
        }

        _forkedOk  = true;
        _childPids = started;
        _tracker   = finalTracker;

        munmap(slots, shmSize);

        for (long i = 0; i < _tracker.getDimension(); i++)
            _tracker.setName(i, StringParser::parseNumber<long>(i+1));

        if (started.size() + 1 != numSlices)
        {
            logMessage(Logger::Level::WARNING)
                << "processos efetivos: " << (started.size() + 1)
                << " de " << numSlices << " pedidos (fork limitado pelo SO)";
        }
        return 0;
    }

    /* ---------------------------------------------------------------------
     * readline: idêntica ao so_sequencial.
     * ------------------------------------------------------------------- */
    static std::string readline(std::istream & stream)
    {
        std::string line;
        std::istream::sentry guard(stream, true);
        std::streambuf * buffer = stream.rdbuf();

        while (true)
        {
            int character = buffer->sbumpc();
            if (character == StatisticsApp::LF) return line;
            if (character == StatisticsApp::CR)
            {
                character = buffer->sgetc();
                if (character == StatisticsApp::LF) buffer->sbumpc();
                return line;
            }
            if (character == StatisticsApp::EF)
            {
                if (line.empty()) stream.setstate(std::ios::eofbit);
                return line;
            }
            line += static_cast<char>(character);
        }
    }

    static const int LF = '\n';
    static const int CR = '\r';
    static const int EF = EOF;

private:
    CommandLineOptions _options;
    std::ostream * _outputStream;
    MultivariateTracker _tracker;
    bool _forkedOk = false;
    std::vector<pid_t> _childPids;
};


namespace AppStatus
{
    enum Status
    {
        SUCCESS = 0,
        FAILED_PARSING,
        FAILED_RUN,
        FAILED_DISPLAY
    };
}

std::string readTextOrDefault(std::string const & defaultValue)
{
    std::string input;
    std::getline(std::cin, input);
    if (input.empty()) return defaultValue;
    return input;
}

unsigned int readWholeNumberOrDefault(unsigned int defaultValue)
{
    while (true)
    {
        std::string input = readTextOrDefault(StringParser::parseNumber<unsigned int>(defaultValue));
        try
        {
            int parsed = std::stoi(input);
            if (parsed < 0)
            {
                std::cout << "  Digite um número maior ou igual a zero.\n> ";
                continue;
            }
            return static_cast<unsigned int>(parsed);
        }
        catch (std::exception const &)
        {
            std::cout << "  Número inválido. Tente novamente.\n> ";
        }
    }
}

void interactiveMenu(CommandLineOptions &options)
{
    std::cout << "\n=== Menu Interativo (Enter = valor entre [ ]) ===\n";

    std::cout << "Arquivo de entrada (vazio = teclado): ";
    options.fileInput = readTextOrDefault("");

    std::cout << "Linhas de cabeçalho a pular [1]: ";
    options.numLinesToSkip = readWholeNumberOrDefault(1);

    std::cout << "Número de processos [4]: ";
    options.numProcesses = readWholeNumberOrDefault(4);
    if (options.numProcesses < 1) options.numProcesses = 1;

    std::cout << "Colunas (ex: 1,2,3 ou 1:4) [1,2,3,4]: ";
    std::string columnsInput = readTextOrDefault("1,2,3,4");

    std::cout << "Delimitador [,]: ";
    std::string delim = readTextOrDefault(",");
    if (delim.size() != 1) delim = ",";
    options.delimiter = delim;

    std::vector<int> filters;
    for (std::string const & token : StringSplitter(columnsInput, ",").tokens())
    {
        std::vector<int> range;
        if (StringSplitter::toIntegers(token, range))
        {
            for (int entry : range)
            {
                if (entry > 0) filters.push_back(entry - 1);
            }
        }
    }

    if (!filters.empty())
    {
        int maxFilter = *std::max_element(filters.begin(), filters.end());
        options.filterColumns.assign(maxFilter + 1, false);
        for (int f : filters) options.filterColumns.at(f) = true;
    }

    options.showStatistics = true;
    options.statisticsOptions.doMax = true;
    options.statisticsOptions.doMean = true;
    options.statisticsOptions.doMin = true;
    options.statisticsOptions.doVar = true;
    options.useFork = (options.numProcesses > 1);

    std::cout << "\n";
}

int pdrv(int argc, char * argv[])
{
    CommandLineOptions options;

    if (argc <= 1)
    {
        interactiveMenu(options);
    }
    else
    {
        try
        {
            CommandLineParser parser(argc, argv);
            if (parser.showUsage())
            {
                CommandLineParser::printUsage();
                return AppStatus::SUCCESS;
            }
            else if (parser.showVersion())
            {
                CommandLineParser::printVersion();
                return AppStatus::SUCCESS;
            }
            options = parser.options;
        }
        catch (std::exception & ex)
        {
            logMessage(Logger::Level::FATAL) << ex.what();
            return AppStatus::FAILED_PARSING;
        }
    }

    StatisticsApp application(options);

    try
    {
        auto t0 = std::chrono::steady_clock::now();
        int status = application.run();
        if (status != 0) return AppStatus::FAILED_RUN;
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();

        std::ostringstream info;
        info << std::fixed << std::setprecision(4);
        if (application.usedFork())
        {
            info << "[RESUMO] Modo: PARALELO | processos: "
                 << (application.childPids().size() + 1)
                 << " (pai PID " << getpid() << " + "
                 << application.childPids().size() << " filhos, PIDs:";
            for (pid_t c : application.childPids()) info << " " << c;
            info << ")";
        }
        else
        {
            info << "[RESUMO] Modo: SEQUENCIAL | processos: 1 (PID " << getpid() << ")";
            if (options.numProcesses > 1)
                info << " — pedido -p " << options.numProcesses
                     << ", mas o fork NAO foi usado (fallback)";
        }
        info << " | tempo de processamento: " << secs << " s";
        std::cerr << info.str() << std::endl;
    }
    catch (std::exception & ex)
    {
        logMessage(Logger::Level::FATAL) << ex.what();
        return AppStatus::FAILED_RUN;
    }

    try
    {
        bool isDisplayValid = application.display();
        if (!isDisplayValid) return AppStatus::FAILED_DISPLAY;
    }
    catch (std::exception & ex)
    {
        logMessage(Logger::Level::FATAL) << ex.what();
        return AppStatus::FAILED_DISPLAY;
    }

    return AppStatus::SUCCESS;
}

#ifndef _CLISTATS_TESTING
int main(int argc, char * argv[])
{
    return pdrv(argc, argv);
}
#endif