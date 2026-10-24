/* Cabeçalhos padrão da biblioteca C++ */
#include <algorithm>   // Para std::max_element, std::swap, etc.
#include <chrono>      // Para std::chrono::steady_clock (medição de tempo)
#include <cmath>       // Para std::isnan, sqrt, etc.
#include <fstream>     // Para std::ifstream (leitura de arquivos)
#include <iomanip>     // Para std::setw, std::setfill (formatação de saída)
#include <iostream>    // Para std::cout, std::cerr, std::cin
#include <limits>      // Para std::numeric_limits
#include <list>        // Para std::list (usado no parser de argumentos)
#include <numeric>     // Para std::accumulate
#include <sstream>     // Para std::stringstream, std::istringstream
#include <stdexcept>   // Para std::runtime_error
#include <string>      // Para std::string
#include <vector>      // Para std::vector

#include <unistd.h>    // Para getpid()


struct ApplicationProperties
{
 
    static
    std::string
    version()
    {
        return std::string(ApplicationProperties::VERSION_MAJOR) + "." +
            ApplicationProperties::VERSION_MINOR + "." +
            ApplicationProperties::VERSION_PATCH;
    }

    // Nome do programa
    static constexpr const char * NAME = "clistats";
    // Email do autor para reportar bugs
    static constexpr const char * AUTHOR = "dpmcmlxxvi@gmail.com";
    // Componentes da versão (versionamento semântico)
    static constexpr const char * VERSION_MAJOR = "1";
    static constexpr const char * VERSION_MINOR = "1";
    static constexpr const char * VERSION_PATCH = "0";
};

/**
 * @struct Logger
 * @brief Logger simples (sistema de registro de mensagens)
 * @details Deve ser invocado via "logMessage(Logger::Level, ...) << "mensagem";"
 *
 * Apenas mensagens com nível igual ou inferior ao nível global
 * configurado serão exibidas.
 */
struct Logger
{

    /**
     * @struct Level
     * @brief Níveis de severidade das mensagens de log
     *
     * FATAL é o mais severo (0), DETAIL é o menos severo (5).
     */
    struct Level
    {
        enum Type
        {
            FATAL = 0,    // Erros fatais que impedem a execução
            ERROR,        // Erros que podem ser recuperados
            WARNING,      // Avisos sobre possíveis problemas
            INFO,         // Informações gerais sobre o progresso
            DEBUG,        // Informações de depuração
            DETAIL        // Informações muito detalhadas (máximo verboso)
        };
    };

    /**
     * Registra uma mensagem de log.
     * O nível do log é escrito na linha atual do stream e então o stream
     * é retornado. O usuário deve adicionar o final de linha (EOL).
     *
     * @return Stream de logging correspondente ao nível de log
     */
    static std::ostream & log(Logger::Level::Type level)
    {
        // Converte o nível numérico para uma string legível
        std::string label = (level==Logger::Level::FATAL  ? "FATAL":
                            (level==Logger::Level::ERROR  ? "ERROR":
                            (level==Logger::Level::WARNING? "WARNING":
                            (level==Logger::Level::INFO   ? "INFO":
                            (level==Logger::Level::DEBUG  ? "DEBUG":"DETAIL")))));

        // Níveis FATAL, ERROR e WARNING vão para stderr (erro padrão)
        // Níveis INFO, DEBUG e DETAIL vão para stdout (saída padrão)
        std::ostream * os = (level <= Logger::Level::WARNING ? &std::cerr : &std::cout);

        // Escreve o rótulo alinhado à esquerda com largura 7, seguido de ": "
        *os << std::left << std::setw(7) << label << ": ";
        return *os;
    }

    /**
     * Nível de log global da aplicação (por padrão FATAL)
     */
    static Level::Type logLevel;

};

// Inicializa a variável estática (necessário em C++ para membros estáticos)
Logger::Level::Type Logger::logLevel = Logger::Level::FATAL;

/**
 * @class LogStream
 * @brief Transmite uma mensagem de log para a saída do logger se seu nível estiver habilitado
 * @details Retornado por #logMessage. Valores inseridos com operator<< são
 *          escritos apenas se o nível fornecido estiver habilitado; o stream
 *          é terminado com um final de linha quando o temporário é destruído.
 */
class LogStream
{
public:

    /**
     * Inicializa o stream de log para o nível fornecido
     * @param[in] level Nível de log desta mensagem
     */
    explicit
    LogStream(Logger::Level::Type level) :
        _enabled(level <= Logger::logLevel),  // Habilita se nível <= nível global
        _stream(0)                             // Ponteiro nulo inicialmente
    {
        if (this->_enabled)
        {
            // Obtém o stream apropriado do Logger
            this->_stream = &Logger::log(level);
        }
    }

    /**
     * Termina a mensagem com um final de linha se o nível estava habilitado
     */
    ~LogStream()
    {
        if (this->_enabled)
        {
            // Adiciona quebra de linha e faz flush
            *(this->_stream) << std::endl;
        }
    }

    /**
     * Transmite um valor para a mensagem de log
     * @param[in] value Valor a ser transmitido
     * @return Referência para este stream de log
     */
    template <class T>
    LogStream &
    operator<<(T const & value)
    {
        if (this->_enabled)
        {
            *(this->_stream) << value;
        }
        return *this;  // Retorna referência para permitir encadeamento
    }

private:

    bool _enabled;          // Se esta mensagem deve ser exibida
    std::ostream * _stream; // Ponteiro para o stream de saída

};

/**
 * Cria um stream de log para o nível fornecido
 * @param[in] level Nível de log desta mensagem
 * @return Stream de log que escreve na saída do logger apenas se o nível estiver habilitado
 */
inline
LogStream
logMessage(Logger::Level::Type level)
{
    return LogStream(level);
}


/**
 * @class StringParser
 * @brief Métodos de parsing (análise) de strings
 */
class StringParser
{

public:

    /**
     * Converte número para string (formato fixo)
     * @param[in] value Valor a ser convertido
     * @return Representação em string do valor de entrada
     *
     * Trata NaN (Not a Number) consistentemente retornando "nan".
     */
    template <class T>
    static
    std::string
    parseNumber(T value)
    {
        // Verifica NaN para retornar consistentemente "nan"
        if (std::isnan(static_cast<double>(value))) return "nan";
        std::string entry;
        std::stringstream parser;
        parser << std::fixed << value;
        parser >> entry;
        return entry;
    }

    /**
     * Converte string para valor do tipo template
     * @param[in] src String de origem
     * @param[out] dst Valor de destino
     * @return True se o parsing foi bem-sucedido, caso contrário false
     *
     * Verifica se toda a string foi consumida no parsing (não apenas parte dela).
     */
    template <class T>
    static
    bool
    toValue(std::string const & src,
            T & dst)
    {
        std::istringstream parser(src.c_str());
        parser >> dst;
        if (!parser) return false;  // Falha no parsing

        // Verifica se toda a string foi consumida
        return (parser.rdbuf()->in_avail() == 0);
    }

};

/**
 * @class StringSplitter
 * @brief Divide uma string de origem em tokens dado um delimitador
 */
class StringSplitter
{

public:

    /**
     * Tipo de subscrito
     */
    typedef std::vector<std::string>::size_type size_type;

public:

    /**
     * Cria StringSplitter vazio
     */
    StringSplitter()
    {
    }

    /**
     * Cria e inicializa um novo StringSplitter
     * @param[in] src String de origem a dividir
     * @param[in] delim O delimitador para dividir a string
     */
    StringSplitter(std::string const & src,
                   std::string const & delim)
    {
        this->initialize(src, delim);
    }

    /**
     * Recupera o token no índice especificado
     * @param[in] index Índice do token
     * @return O token no índice especificado
     * @throws std::out_of_range se o índice for inválido
     */
    std::string const &
    at(size_type index) const
    {
        return this->_tokens.at(index);
    }

    /**
     * Inicializa o divisor com uma nova origem e delimitador
     * @param[in] src A string a dividir
     * @param[in] delim O delimitador para dividir a string
     */
    void
    initialize(std::string const & src,
               std::string const & delim)
    {
        std::vector<std::string> tokens;
        std::string::size_type startpos = 0;  // Posição inicial do token atual
        std::string::size_type endpos = 0;    // Posição final do token atual

        while (true)
        {
            // Encontra o próximo delimitador e extrai o token
            endpos = src.find(delim, startpos);
            tokens.push_back(src.substr(startpos, endpos - startpos));

            // Se endpos é npos, copiamos o último token
            if (endpos == std::string::npos) break;

            // Exclui o delimitador da próxima busca
            startpos = endpos + delim.size();
        }

        // Troca o vetor antigo pelo novo (eficiente)
        this->_tokens.swap(tokens);
    }

    /**
     * Recupera o número de tokens divididos
     * @return O número de tokens divididos
     */
    size_type
    size() const
    {
        return this->_tokens.size();
    }

    /**
     * Recupera todos os tokens
     * @return Vetor de tokens divididos
     */
    std::vector<std::string>
    tokens() const
    {
        return this->_tokens;
    }

    /**
     * Converte token para valor do tipo template
     * @param[in] index Índice do token
     * @param[out] dst Valor de destino
     * @return True se o parsing foi bem-sucedido, caso contrário false
     */
    template <class T>
    bool
    toValue(size_type index,
            T & dst) const
    {
        return StringParser::toValue<T>(this->at(index), dst);
    }

    /**
     * Converte string para intervalo de inteiros.
     * O formato esperado é "A:B". Se um intervalo for encontrado (ex: 3:5)
     * então o array de números do primeiro ao segundo é retornado
     * (ex: [3,4,5]). Se nenhum delimitador de intervalo for encontrado,
     * a string é assumida como um único inteiro.
     *
     * @param[in] src String de origem
     * @param[out] dst Vetor de destino ao qual os inteiros são anexados
     * @param[in] delim Delimitador de intervalo opcional. Padrão = ":"
     * @return True se o parsing foi bem-sucedido, caso contrário false
     */
    static
    bool
    toIntegers(std::string const & src,
               std::vector<int> & dst,
               std::string const & delim = ":")
    {

        int start = 0;
        int stop = 0;

        // Divide a string pelo delimitador de intervalo
        StringSplitter splitter(src, delim);

        if (splitter.size() == 1)
        {
            // Se um token, tenta fazer parsing e define start = stop
            bool good = StringParser::toValue<int>(splitter.at(0), start);
            if (!good) return false;
            stop = start;
        }
        else if (splitter.size() == 2)
        {
            // Se dois tokens, tenta fazer parsing de start e stop
            bool goodStart = StringParser::toValue<int>(splitter.at(0), start);
            if (!goodStart) return false;

            bool goodStop = StringParser::toValue<int>(splitter.at(1), stop);
            if (!goodStop) return false;

            // Troca para que start venha antes de stop
            if (stop < start)
            {
                std::swap(start, stop);
            }
        }
        else
        {
            // Se não for 1 ou 2 tokens, falha
            return false;
        }

        // Cria array de inteiros
        for (int i = start; i < stop+1; i++)
        {
            dst.push_back(i);
        }

        return true;

    }

private:

    /**
     * Contém os tokens divididos
     */
    std::vector<std::string> _tokens;

};

/**
 * @struct DataPoint
 * @brief Um valor numérico com um estado ativo/inativo.
 *
 * O estado ativo indica se o valor é válido e deve ser incluído
 * nos cálculos estatísticos.
 */
struct DataPoint
{
    /**
     * Cria DataPoint padrão: inativo com valor 0
     */
    DataPoint() :
        active(false),
        value(0)
    {
    }

    /**
     * Estado ativo do DataPoint
     */
    bool active;

    /**
     * Valor do DataPoint
     */
    double value;

};

/**
* @class DataVector
* @brief Vetor STL de #DataPoint
*/
class DataVector : public std::vector<DataPoint>
{
public:
    /**
    * @brief Define o estado ativo de todas as entradas como false
    */
    void
    deactivate()
    {
        for (DataPoint & point : *this)
        {
            point.active = false;
        }
    }

    /**
     * Copia subvetor usando máscara
     * @param[in] src Vetor de origem
     * @param[in] numSrc Número de elementos a copiar da origem
     * @param[out] dst Vetor de destino
     * @param[in] numDst Número de elementos no destino
     * @param[in] mask Máscara booleana indicando quais elementos copiar
     * @param[in] numMask Tamanho da máscara
     */
    static
    void copy(DataVector const & src,
              DataVector::size_type const & numSrc,
              DataVector & dst,
              DataVector::size_type const & numDst,
              std::vector<bool> const & mask,
              DataVector::size_type const & numMask)
    {

        // Desativa todos os valores de destino por padrão
        dst.deactivate();

        // Copia elementos
        DataVector::size_type j = 0;  // Índice no destino
        for (DataVector::size_type i = 0; i < numSrc; i++)
        {
            // Verifica se o valor atual está mascarado (deve ser descartado)
            if (i < numMask)
            {
                if (!mask.at(i)) continue;
            }

            // Adiciona ou define o próximo valor
            if (j >= numDst)
            {
                dst.push_back(src.at(i));  // Destino cheio, adiciona
            }
            else
            {
                dst.at(j) = src.at(i);     // Sobrescreve posição existente
            }
            j++;
        }

    }
};

/**
 * @struct StatisticsTrackerOptions
 * @brief Opções de quais estatísticas calcular
 */
struct StatisticsTrackerOptions
{
    StatisticsTrackerOptions()
    {
        this->doMax = false;   // Calcular máximo?
        this->doMean = false;  // Calcular média?
        this->doMin = false;   // Calcular mínimo?
        this->doVar = false;   // Calcular variância?
    }
    bool doMax;
    bool doMean;
    bool doMin;
    bool doVar;
};

/**
 * @struct CommandLineOptions
 * @brief Opções da linha de comando (ou do menu interativo)
 */
struct CommandLineOptions
{
    CommandLineOptions()
    {
        this->delimiter = "";                    // Delimitador de campos
        this->fileInput = "";                    // Arquivo de entrada
        this->filterColumns = std::vector<bool>(); // Máscara de colunas
        this->numLinesToKeep = std::numeric_limits<int>::max(); // Linhas a processar
        this->numLinesToSkip = 0;                // Linhas a pular (cabeçalho)
        this->showFilteredData = false;          // Mostrar dados filtrados?
        this->showStatistics = false;            // Mostrar estatísticas?
        this->verboseLevel = 0;                  // Nível de verbosidade
    }
    std::string delimiter;              // Delimitador de campos (ex: ",")
    std::string fileInput;              // Caminho do arquivo de entrada
    std::vector<bool> filterColumns;    // Máscara de colunas a processar
    unsigned int numLinesToKeep;        // Número máximo de linhas a processar
    unsigned int numLinesToSkip;        // Número de linhas a pular
    bool showFilteredData;              // Exibir dados após filtragem
    bool showStatistics;                // Exibir estatísticas resumidas
    StatisticsTrackerOptions statisticsOptions; // Quais estatísticas calcular
    unsigned char verboseLevel;         // Nível de verbosidade (0-5)
};

/**
 * @class CommandLineParser
 * @brief Analisa argumentos da linha de comando
 *
 * Converte os argumentos argv em um objeto CommandLineOptions.
 * Lança exceções se os argumentos forem inválidos.
 */
class CommandLineParser
{

public:
    /**
     * Cria parser de linha de comando
     * @param[in] argc Contagem de argumentos
     * @param[in] argv Array de argumentos
     * @throws std::runtime_error se o parsing for inválido
     */
    CommandLineParser(int argc,
                      char * argv[]) :
      _showUsage(false),
      _showVersion(false)
    {
        this->parse(argc, argv);
    }

    /**
     * Imprime instruções de uso do software
     */
    static
    void
    printUsage()
    {
        std::cout << std::endl;
        std::cout << "NAME" << std::endl;
        std::cout << std::endl;
        std::cout << "    clistats - Command line statistics tool" << std::endl;
        std::cout << std::endl;
        std::cout << "SYNOPSIS" << std::endl;
        std::cout << std::endl;
        std::cout << "    clistats [-h] [-v <level>] [-V] [-i <file>] [-s <rows>]" << std::endl;
        std::cout << "             [-fc <filter>] [-fd] [-ss] < [file]" << std::endl;
        std::cout << std::endl;
        std::cout << "DESCRIPTION" << std::endl;
        std::cout << std::endl;
        std::cout << "    A command line tool to compute statistics for a stream of delimited input" << std::endl;
        std::cout << "    numbers. It takes a stream of numbers from standard input or a redirected" << std::endl;
        std::cout << "    file. To stop processing and display the statistics enter the EOF signal" << std::endl;
        std::cout << "    (CTRL-D on POSIX systems like Linux or Cygwin or CTRL-Z on Windows)." << std::endl;
        std::cout << "    The default delimiter is a comma." << std::endl;
        std::cout << std::endl;
        std::cout << "    Delimited tokens that are not valid numeric values are skipped." << std::endl;
        std::cout << std::endl;
        std::cout << "    Note, all statistics are computed on a rolling basis and not by using the" << std::endl;
        std::cout << "    entire dataset, so all non-order statistics are approximations." << std::endl;
        std::cout << std::endl;
        std::cout << "    Displayed statistics use headers that start with # to enable piping" << std::endl;
        std::cout << "    results to gnuplot." << std::endl;
        std::cout << std::endl;
        std::cout << "    Available statistics to display are:" << std::endl;
        std::cout << std::endl;
        std::cout << "        - Count" << std::endl;
        std::cout << "        - Minimum" << std::endl;
        std::cout << "        - Mean" << std::endl;
        std::cout << "        - Maximum" << std::endl;
        std::cout << "        - Standard deviation" << std::endl;
        std::cout << std::endl;
        std::cout << "OPTIONS" << std::endl;
        std::cout << std::endl;
        std::cout << "    General options:" << std::endl;
        std::cout << std::endl;
        std::cout << "        -h" << std::endl;
        std::cout << "        --help" << std::endl;
        std::cout << "            Print this help message" << std::endl;
        std::cout << std::endl;
        std::cout << "        -v <level>" << std::endl;
        std::cout << "        --verbose <level>" << std::endl;
        std::cout << "            Verbose level (0 to 5). Default = 0." << std::endl;
        std::cout << std::endl;
        std::cout << "        -V" << std::endl;
        std::cout << "        --version" << std::endl;
        std::cout << "            Software version is displayed." << std::endl;
        std::cout << std::endl;
        std::cout << "    I/O options:" << std::endl;
        std::cout << std::endl;
        std::cout << "        -i <file>" << std::endl;
        std::cout << "        --input <file>" << std::endl;
        std::cout << "            Input file. Default is stdin." << std::endl;
        std::cout << std::endl;
        std::cout << "        file" << std::endl;
        std::cout << "            Input file can be redirected as standard input" << std::endl;
        std::cout << std::endl;
        std::cout << "    Parsing options:" << std::endl;
        std::cout << std::endl;
        std::cout << "        -fc <filter>" << std::endl;
        std::cout << "        --filterColumn" << std::endl;
        std::cout << "            Filter columns to process using comma delimited array of column" << std::endl;
        std::cout << "            indices to keep." << std::endl;
        std::cout << "            Format: \"<c1>,<c2>,...,<c3:c4>,...\"" << std::endl;
        std::cout << "                (e.g., \"1,2,3,5\" will discard column 4). Range of" << std::endl;
        std::cout << "                columns can be indicated using \":\" (e.g., 1:3,5)." << std::endl;
        std::cout << std::endl;
        std::cout << "        -s <rows>" << std::endl;
        std::cout << "        --skip <rows>" << std::endl;
        std::cout << "            Number of rows to skip before processing begins (e.g., header row)" << std::endl;
        std::cout << "            Default = 0 (process all rows)." << std::endl;
        std::cout << std::endl;
        std::cout << "    Display options:" << std::endl;
        std::cout << std::endl;
        std::cout << "        -fd" << std::endl;
        std::cout << "        --filtered" << std::endl;
        std::cout << "            Show filtered data to be processed." << std::endl;
        std::cout << std::endl;
        std::cout << "        -ss" << std::endl;
        std::cout << "        --statistics" << std::endl;
        std::cout << "            Show summary statistics. Enabled by default unless another display" << std::endl;
        std::cout << "            option is enabled." << std::endl;
        std::cout << std::endl;
        std::cout << "EXIT STATUS" << std::endl;
        std::cout << std::endl;
        std::cout << "    0 = Success" << std::endl;
        std::cout << "    1 = Argument parsing failed" << std::endl;
        std::cout << "    2 = Computing statistics failed" << std::endl;
        std::cout << "    3 = Displaying statistics failed" << std::endl;
        std::cout << std::endl;
        std::cout << "BUGS" << std::endl;
        std::cout << std::endl;
        std::cout << "    Report bugs to " << ApplicationProperties::AUTHOR << std::endl;
    }

    /**
     * Imprime versão do software
     */
    static
    void
    printVersion()
    {
        std::cout << ApplicationProperties::version() << std::endl;
    }

    /**
     * Flag de uso da aplicação
     * @return True se o uso foi solicitado, caso contrário false
     */
    bool
    showUsage() const noexcept
    {
        return this->_showUsage;
    }

    /**
     * Flag de versão da aplicação
     * @return True se a versão foi solicitada, caso contrário false
     */
    bool
    showVersion() const noexcept
    {
        return this->_showVersion;
    }

private:

    /**
     * Analisa argumentos da linha de comando
     * @param[in] argc Contagem de argumentos
     * @param[in] argv Array de argumentos
     * @throws std::runtime_error se o argumento da flag não for reconhecido
     *         ou se o parsing do valor falhar
     */
    void
    parse(int argc,
          char * argv[])
    {

        // Itera por cada argumento até que todos sejam analisados
        // Se o argumento for reconhecido como uma flag válida, ele é analisado
        //      Se o parsing falhar no valor da flag, uma exceção é lançada
        // Se o argumento não for reconhecido como uma flag válida, uma exceção é lançada
        std::list<std::string> arguments(argv, argv + argc);
        arguments.pop_front(); // Remove o nome do executável

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
                // Analisa o valor da flag de verbosidade para um único inteiro
                // dentro do intervalo dos níveis de log
                unsigned int verbose = 0;
                std::string value = "";
                if ((++argument) != arguments.end())
                {
                    value = *argument;
                }
                bool isInt = StringParser::toValue<unsigned int>(value, verbose);
                if (!(isInt && verbose <= Logger::Level::DETAIL))
                {
                    throw std::runtime_error("Nível de verbosidade inválido");
                }
                this->options.verboseLevel = (unsigned char)verbose;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-i" || flag == "--input")
            {
                // Analisa o valor da flag de arquivo de entrada para uma única string
                std::string value = "";
                if ((++argument) != arguments.end())
                {
                    value = *argument;
                }
                std::ifstream f(value.c_str());
                if (!f.good())
                {
                    throw std::runtime_error("Arquivo de entrada inválido");
                }
                this->options.fileInput = value;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-s" || flag == "--skip")
            {
                // Analisa o valor da flag de pular para um único inteiro não negativo
                int skip = 0;
                std::string value = "";
                if ((++argument) != arguments.end())
                {
                    value = *argument;
                }
                bool isInt = StringParser::toValue<int>(value, skip);
                if (!(isInt && skip >= 0))
                {
                    throw std::runtime_error("Número inválido de linhas para pular");
                }
                this->options.numLinesToSkip = skip;
                arguments.pop_front();
                arguments.pop_front();
            }
            else if (flag == "-fc" || flag == "--filterColumn")
            {
                // Analisa o valor da flag de filtro para um vetor de inteiros
                // positivos delimitado por vírgulas
                std::string value = "";
                if ((++argument) != arguments.end())
                {
                    value = *argument;
                }

                // Analisa filtros em tokens delimitados
                StringSplitter parser(value, ",");
                std::vector<std::string> values = parser.tokens();

                std::vector<int> filters;
                for (unsigned int j = 0; j < values.size(); j++)
                {
                    // Analisa string para inteiros e verifica intervalos (ex: 3:5)
                    std::vector<int> range;
                    bool isRangeValid = StringSplitter::toIntegers(values.at(j), range);
                    if (!isRangeValid)
                    {
                        throw std::runtime_error("Valor de coluna de filtro inválido = " + values.at(j));
                    }

                    // Verifica se cada entrada do intervalo é positiva
                    for (int entry : range)
                    {
                        if (entry <= 0)
                        {
                            throw std::runtime_error("Valor de coluna de filtro inválido = " + values.at(j));
                        }
                        filters.push_back(entry-1);  // Converte para índice base 0
                    }

                }

                // Popula a máscara de filtro
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
            else
            {
                throw std::runtime_error("Flag não reconhecida (" + flag + "). Execute \"clistats --help\" para uso.");
            }

        }

        // Verifica se o delimitador foi fornecido
        if (this->options.delimiter.empty())
        {
            this->options.delimiter = ",";  // Padrão: vírgula
        }

        // Verifica se estatísticas básicas padrão devem ser mostradas
        if (!this->options.showStatistics)
        {
            this->options.showStatistics = !(this->options.showFilteredData);
        }

        // Determina quais estatísticas devem ser calculadas
        this->options.statisticsOptions.doMax = this->options.showStatistics;
        this->options.statisticsOptions.doMean = this->options.showStatistics;
        this->options.statisticsOptions.doMin = this->options.showStatistics;
        this->options.statisticsOptions.doVar = this->options.showStatistics;

    }

public:

    /**
     * Opções
     */
    CommandLineOptions options;

private:

    /**
     * Flag de exibição de uso da aplicação
     */
    bool _showUsage;

    /**
     * Flag de exibição de versão da aplicação
     */
    bool _showVersion;

};

/**
 * @class StatisticsTracker
 * @brief Rastreia as estatísticas de uma variável escalar.
 *
 * Implementa o algoritmo de Welford para cálculo incremental
 * de média e variância, que é numericamente estável.
 */
class StatisticsTracker
{

public:

    /**
     * Construtor
     * @param[in] options Opções de estatísticas
     */
    explicit
    StatisticsTracker(StatisticsTrackerOptions const & options) :
        _count(0),
        _maximum(0),
        _mean(0),
        _minimum(0),
        _name(""),
        _options(options),
        _variance(0)
    {
    }

    /**
     * Obtém contagem de dados
     */
    long
    getCount() const noexcept
    {
        return this->_count;
    }

    /**
     * Obtém mínimo dos dados
     */
    double
    getMinimum() const noexcept
    {
        return this->_minimum;
    }

    /**
     * Obtém média dos dados
     */
    double
    getMean() const noexcept
    {
        return this->_mean;
    }

    /**
     * Obtém máximo dos dados
     */
    double
    getMaximum() const noexcept
    {
        return this->_maximum;
    }

    /**
     * Obtém nome dos dados
     */
    std::string
    getName() const
    {
        return this->_name;
    }

    /**
     * Obtém variância dos dados
     */
    double
    getVariance() const noexcept
    {
        return this->_variance;
    }

    /**
     * Define nome dos dados
     * @param[in] name Nome dos dados
     */
    void
    setName(std::string const & name)
    {
        this->_name = name;
    }

    /**
     * Atualiza estatísticas com novo valor de dados
     * @param[in] value Valor do dado
     *
     * Algoritmo de Welford:
     *   delta = value - mean_old
     *   mean_new = mean_old + delta / count
     *   variance_new = ((count-1) * variance_old + delta * (value - mean_new)) / count
     */
    void
    update(double const value)
    {

        // Atualiza contador
        this->_count++;

        // Inicializa se for o primeiro valor
        if (this->_count == 1)
        {
            if (this->_options.doMin)
            {
                this->_minimum = value;
            }
            if (this->_options.doMax)
            {
                this->_maximum = value;
            }
            if (this->_options.doMean)
            {
                this->_mean = value;
            }
            if (this->_options.doVar)
            {
                this->_variance = 0;  // Variância de um único ponto é 0
            }
            return;
        }

        // Atualiza mínimo
        if (this->_options.doMin)
        {
            if (value < this->_minimum)
            {
                this->_minimum = value;
            }
        }

        // Atualiza máximo
        if (this->_options.doMax)
        {
            if (value > this->_maximum)
            {
                this->_maximum = value;
            }
        }

        if (this->_options.doMean || this->_options.doVar)
        {
            double delta = value - this->_mean;

            if (this->_options.doMean)
            {
                // Atualiza média
                this->_mean += delta / static_cast<double>(this->_count);
            }

            if (this->_options.doVar)
            {
                // Atualiza variância
                // Incremento de Welford (delta contém a média antiga, _mean já é a nova)
                double count = static_cast<double>(this->_count);
                this->_variance = (count-1) * this->_variance + delta * (value - this->_mean);
                this->_variance /= count;
            }
        }

    }

protected:

    /**
     * Contagem da estatística
     */
    long _count;

    /**
     * Máximo da estatística
     */
    double _maximum;

    /**
     * Média da estatística
     */
    double _mean;

    /**
     * Mínimo da estatística
     */
    double _minimum;

    /**
     * Nome da estatística
     */
    std::string _name;

    /**
     * Opções de estatísticas
     */
    StatisticsTrackerOptions _options;

    /**
     * Variância da estatística
     */
    double _variance;

};

/**
 * @class MultivariateTracker
 * @brief Rastreia as estatísticas de uma variável multivariada.
 *
 * Gerencia múltiplos StatisticsTracker, um para cada dimensão/coluna
 * do conjunto de dados.
 */
class MultivariateTracker
{

public:

    /**
     * Construtor
     * @param[in] size Tamanho dimensional das estatísticas a rastrear
     * @param[in] options Opções de estatísticas
     */
    MultivariateTracker(long size,
                        StatisticsTrackerOptions const & options) :
        _options(options)
    {
        this->initialize(size, options);
    }

    /**
     * Obtém contagem de dados
     * @param[in] index Índice da multi-variável
     */
    long
    getCount(long const index) const
    {
        return this->_statistics[index].getCount();
    }

    /**
     * Obtém dimensão do rastreador
     */
    long
    getDimension() const noexcept
    {
        return static_cast<long>(this->_statistics.size());
    }

    /**
     * Obtém mínimo dos dados
     * @param[in] index Índice da multi-variável
     */
    double
    getMinimum(long const index) const
    {
        return this->_statistics[index].getMinimum();
    }

    /**
     * Obtém média dos dados
     * @param[in] index Índice da multi-variável
     */
    double
    getMean(long const index) const
    {
        return this->_statistics[index].getMean();
    }

    /**
     * Obtém máximo dos dados
     * @param[in] index Índice da multi-variável
     */
    double
    getMaximum(long const index) const
    {
        return this->_statistics[index].getMaximum();
    }

    /**
     * Obtém nome dos dados
     * @param[in] index Índice da multi-variável
     */
    std::string
    getName(long const index) const
    {
        return this->_statistics[index].getName();
    }

    /**
     * Obtém variância dos dados
     * @param[in] index Índice da multi-variável
     */
    double
    getVariance(long const index) const
    {
        return this->_statistics[index].getVariance();
    }

    /**
     * Define nome dos dados
     * @param[in] index Índice da multi-variável
     * @param[in] name Nome dos dados
     */
    void
    setName(long const index,
            std::string const & name)
    {
        this->_statistics[index].setName(name);
    }

    /**
     * Atualiza estatísticas com novos pontos de dados
     * @param[in] data Novo ponto de dados
     * @return True se a estatística foi atualizada, caso contrário false
     *
     * Verifica se as dimensões dos dados correspondem às dimensões do rastreador.
     * Se o rastreador estiver vazio, inicializa com o tamanho dos dados.
     */
    bool
    update(DataVector const & data)
    {

        // Verifica se as dimensões dos dados correspondem às dimensões do rastreador
        if (this->_statistics.size() != 0 && data.size() != this->_statistics.size())
        {
            return false;
        }

        // Tenta inicializar o rastreador
        if (this->_statistics.size() == 0)
        {
            if (data.size() == 0) return false;
            this->initialize(static_cast<long>(data.size()), this->_options);
        }

        // Atualiza estatísticas com cada ponto de dados
        auto datum = data.begin();
        auto tracker = this->_statistics.begin();
        for ( ; tracker != this->_statistics.end(); ++tracker, ++datum)
        {
            if (datum->active) tracker->update(datum->value);  // Só atualiza se ativo
        }

        return true;

    }

private:

    /**
     * Inicializa o array de estatísticas
     * @param[in] size Tamanho do array de estatísticas
     * @param[in] options Opções de estatísticas
     */
    void
    initialize(long size,
               StatisticsTrackerOptions const & options)
    {
        // Inicializa rastreadores de estatísticas escalares
        for (long i = 0 ; i < size; ++i)
        {
            this->_statistics.push_back(StatisticsTracker(options));
        }
    }

protected:

    /**
     * Opções de estatísticas
     */
    StatisticsTrackerOptions _options;

    /**
     * Rastreadores de estatísticas
     */
    std::vector<StatisticsTracker> _statistics;

};

/**
 * @class StatisticsWriter
 * @brief Escreve dados e estatísticas em um stream.
 */
class StatisticsWriter
{
public:

    /**
     * @brief Construtor
     * @param[in] comment Caractere usado para comentar linhas de cabeçalho. Padrão "#".
     */
    StatisticsWriter(std::string const & comment = "#") :
        _comment(comment)
    {
    }

    /**
     * Escreve vetor de dados no stream
     * @param[out] stream Stream de destino
     * @param[in] data Vetor de dados de origem a ser escrito
     * @param[in] delimiter Delimitador de saída dos dados
     *
     * Escreve apenas os valores ativos, separados pelo delimitador.
     */
    void
    writeData(std::ostream & stream,
              DataVector const & data,
              std::string const & delimiter = " ")
    {

        for (auto it = data.begin(); it != data.end();)
        {
            // Pula dados inativos
            if (!it->active)
            {
                ++it;
                continue;
            }

            // Escreve o dado
            stream << it->value;
            ++it;

            // Escreve o delimitador se não for a última entrada
            if (it != data.end())
            {
                stream << delimiter;
            }
        }
        stream << std::endl;

    }

    /**
     * Imprime estatísticas resumidas
     * @param[out] stream Stream de destino
     * @param[in] tracker Rastreador de origem
     *
     * Exibe uma tabela formatada com as colunas:
     * Dimensão | Contagem | Mínimo | Média | Máximo | Desvio Padrão
     */
    void
    writeStatistics(std::ostream & stream,
                    MultivariateTracker & tracker)
    {

        const long numDim = tracker.getDimension();

        // Calcula larguras das colunas
        std::vector<int> width(6,0);
        std::string titles[6] = {"Dimension","Count","Minimum","Mean","Maximum","Stdev"};
        for (int k = 0; k < 6; k++)
        {
            width[k] = static_cast<int>(titles[k].size());
        }
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
        for (int & entry : width)
        {
            entry += 3;  // Adiciona espaçamento
            totalWidth += entry;
        }

        // Imprime cabeçalhos de dados
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

        // Imprime dados
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

    std::string _comment;  // Caractere de comentário (padrão "#")

};

/**
 * @class StatisticsApp
 * @brief Aplicação de estatísticas
 *
 * Coordena todo o fluxo do programa:
 * 1. Configura streams de entrada/saída
 * 2. Lê e processa cada linha de dados
 * 3. Atualiza as estatísticas
 * 4. Exibe os resultados
 */
class StatisticsApp
{

public:

    /**
     * Cria aplicação
     * @param[in] options Opções configuradas
     */
    StatisticsApp(CommandLineOptions const & options) :
        _options(options),
        _outputStream(0),
        _tracker(MultivariateTracker(0, options.statisticsOptions))
    {
        // Define o nível de log global
        Logger::logLevel = (Logger::Level::Type)this->_options.verboseLevel;

        // Define stream de saída
        this->_outputStream = &std::cout;
    }

    /**
     * Executa aplicação
     * @return Código de status
     */
    int
    run()
    {

        // ==================================================
        // Parâmetros da aplicação
        // --------------------------------------------------
        int numDim = 0;      // Número de dimensões/colunas
        int numDimMask = 0;  // Número de dimensões após filtro

        // ==================================================
        // Define stream de entrada
        // --------------------------------------------------
        std::ifstream fileInput(this->_options.fileInput.c_str(), std::ios::in);
        // Usa arquivo se válido, senão usa entrada padrão (cin)
        std::istream * source = (fileInput.good() ? &fileInput : &std::cin);

        logMessage(Logger::Level::INFO) << ApplicationProperties::NAME << " está iniciando";

        // ==================================================
        // Define escritor de saída
        // --------------------------------------------------
        StatisticsWriter writer;

        // ==================================================
        // Loop pelos dados de origem
        // --------------------------------------------------
        StringSplitter parser;

        // Contador usado para rastrear leituras de linhas
        unsigned int counter = 0;

        // DataVector contém os dados analisados de cada linha.
        // Seu tamanho será definido no primeiro parsing bem-sucedido.
        DataVector data;
        DataVector dataMasked;

        while (true)
        {

            // ==================================================
            // Extrai linha atual
            // --------------------------------------------------
            std::string line = StatisticsApp::readline(*source);
            if (!(*source).good())
            {
                logMessage(Logger::Level::DETAIL) << "Fim do arquivo encontrado. Saindo.";
                break;
            }

            // ==================================================
            // Verifica se a linha está vazia
            // --------------------------------------------------
            if (line.size() == 0)
            {
                logMessage(Logger::Level::DETAIL) << "Linha em branco encontrada. Pulando.";
                continue;
            }

            // Prepara contador para parsing
            counter++;

            // ==================================================
            // Pula linhas de cabeçalho
            // --------------------------------------------------
            if (counter <= this->_options.numLinesToSkip)
            {
                logMessage(Logger::Level::INFO) << "Pulando linha de cabeçalho.";
                continue;
            }

            // ==================================================
            // Pula linhas desnecessárias
            // --------------------------------------------------
            if (counter > (this->_options.numLinesToSkip + this->_options.numLinesToKeep))
            {
                logMessage(Logger::Level::INFO) << "Pulando linhas restantes.";
                break;
            }

            // ==================================================
            // Analisa linha atual
            // --------------------------------------------------
            parser.initialize(line, this->_options.delimiter);
            StringSplitter::size_type numTokens = parser.size();
            logMessage(Logger::Level::DETAIL) << "Linha = " << counter << " - conteúdo =\"" << line << "\"";

            // ==================================================
            // Verifica e calcula dimensões dos dados de origem
            // --------------------------------------------------
            bool isDimSet = numDim > 0;
            if (!isDimSet)
            {
                numDim = static_cast<int>(numTokens);
                data.resize(numDim);

                // Garante que só mantemos filtros de coluna se estiverem dentro do número real de colunas
                this->_options.filterColumns.resize(numDim, (this->_options.filterColumns.size() == 0));

                // Calcula quantas colunas são mascaradas
                numDimMask = std::accumulate(this->_options.filterColumns.begin(), this->_options.filterColumns.end(), 0);
                dataMasked.resize(numDimMask);

                logMessage(Logger::Level::INFO) << "Dimensões dos dados definidas para " << numDim;
                logMessage(Logger::Level::INFO) << "Dimensões da máscara definidas para " << numDimMask;
            }

            logMessage(Logger::Level::DEBUG) << "Linha = " << counter << " - Número de tokens encontrados: " << numTokens;

            // ==================================================
            // Converte tokens em array de dados
            // --------------------------------------------------
            for (StringSplitter::size_type i = 0; i < (StringSplitter::size_type) numDim; ++i)
            {

                if (numTokens <= i) break;

                // Tenta converter token para double; se falhar, marca como inativo
                data.at(i).active = parser.toValue<double>(i, data.at(i).value);

                logMessage(Logger::Level::DETAIL) << "Linha = " << counter << " - " << (data.at(i).active ? "Token válido" : "Token inválido") << " = " << parser.at(i);

            }

            // ==================================================
            // Aplica filtros de coluna
            // --------------------------------------------------
            DataVector::copy(data, numDim, dataMasked, numDimMask, this->_options.filterColumns, numDim);

            // ==================================================
            // Escreve dados atuais
            // --------------------------------------------------
            if (this->_options.showFilteredData)
            {
                writer.writeData(*this->_outputStream, dataMasked);
            }

            // ==================================================
            // Atualiza estatísticas com dados atuais
            // --------------------------------------------------
            this->_tracker.update(dataMasked);

            // Como o vetor de dados é pré-alocado, desativamos todas as entradas
            // caso não existam na próxima linha analisada. Desta forma, elas não
            // são inadvertidamente incluídas nas estatísticas da próxima linha.
            data.deactivate();

        }

        // ==================================================
        // Atualiza nomes das estatísticas (índices 1, 2, 3...)
        // --------------------------------------------------
        for (long i = 0; i < numDimMask && i < this->_tracker.getDimension(); i++)
        {
            this->_tracker.setName(i, StringParser::parseNumber<long>(i+1));
        }

        logMessage(Logger::Level::INFO) << ApplicationProperties::NAME << " está completo";

        return 0;

    }

    /**
     * Exibe estatísticas de saída
     * @return True se as estatísticas foram exibidas com sucesso, caso contrário false
     */
    bool
    display()
    {

        StatisticsWriter writer;

        // Mostra estatísticas
        if (this->_options.showStatistics)
        {
            writer.writeStatistics(*this->_outputStream, this->_tracker);
        }

        return true;

    }

private:

    /**
     * @brief Lê linha atual do stream
     * @param[in] stream Stream de origem
     * @return Linha lida como string
     *
     * Trata os três tipos de quebra de linha:
     * LF (\n), CR (\r) e CRLF (\r\n), além de EOF.
     */
    static
    std::string
    readline(std::istream & stream)
    {

        // Linha de saída
        std::string line;

        // Obtém buffer do stream e protege com sentry
        std::istream::sentry guard(stream, true);
        std::streambuf * buffer = stream.rdbuf();

        while (true)
        {

            // Extrai caractere atual e avança ponteiro do stream
            int character = buffer->sbumpc();

            // Verifica se o caractere atual é um line feed
            if (character ==  StatisticsApp::LF)
            {
                return line;
            }

            // Verifica se o caractere atual é um carriage return
            if (character == StatisticsApp::CR)
            {
                // Obtém caractere atual sem avançar ponteiro do stream
                character = buffer->sgetc();
                if(character == StatisticsApp::LF)
                {
                    // Remove line feed do stream e avança ponteiro
                    buffer->sbumpc();
                }
                return line;
            }

            // Verifica se o caractere atual é o fim de arquivo
            if (character == StatisticsApp::EF)
            {
                if(line.empty())
                {
                    stream.setstate(std::ios::eofbit);  // Define estado EOF
                }
                return line;
            }

            // Anexa caractere atual à string de destino
            line += static_cast<char>(character);

        }

    }

private:

    /**
     * Caractere line feed
     */
    static const int LF = '\n';

    /**
     * Caractere carriage return
     */
    static const int CR = '\r';

    /**
     * Caractere End Of File
     */
    static const int EF = EOF;

private:

    /**
     * Opções da aplicação
     */
    CommandLineOptions _options;

    /**
     * Stream de saída
     */
    std::ostream * _outputStream;

    /**
     * Estatísticas
     */
    MultivariateTracker _tracker;

};

/**
 * Códigos de status da aplicação
 */
namespace AppStatus
{
    enum Status
    {
        SUCCESS = 0,        // Sucesso
        FAILED_PARSING,     // Falha no parsing de argumentos
        FAILED_RUN,         // Falha na execução
        FAILED_DISPLAY      // Falha na exibição
    };
}

/**
 * @brief Lê uma linha do console e, se o usuário apenas pressionar Enter,
 *        retorna o valor padrão fornecido em vez da string vazia.
 * @param[in] defaultValue Valor usado quando o usuário não fornece entrada
 * @return O texto digitado pelo usuário, ou defaultValue se nada foi digitado
 */
std::string
readTextOrDefault(std::string const & defaultValue)
{
    std::string input;
    std::getline(std::cin, input);
    if (input.empty())
    {
        return defaultValue;
    }
    return input;
}

/**
 * @brief Lê um número inteiro não negativo do console.
 * @details Continua perguntando novamente se o texto digitado não for um
 *          número válido, em vez de travar o programa.
 * @param[in] defaultValue Valor usado quando o usuário apenas pressionar Enter
 * @return O número inteiro digitado pelo usuário, ou defaultValue
 */
unsigned int
readWholeNumberOrDefault(unsigned int defaultValue)
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

/**
 * @brief Menu interativo curto, usado quando o programa é iniciado sem
 *        argumentos. Enter aceita o valor entre colchetes.
 * @param[out] options Opções preenchidas com as respostas do usuário
 */
void interactiveMenu(CommandLineOptions &options) {

    std::cout << "\n=== Menu Interativo (Enter = valor entre [ ]) ===\n";

    std::cout << "Arquivo de entrada (vazio = teclado): ";
    options.fileInput = readTextOrDefault("");

    std::cout << "Linhas de cabeçalho a pular [1]: ";
    options.numLinesToSkip = readWholeNumberOrDefault(1);

    std::cout << "Colunas (ex: 1,2,3 ou 1:4) [1,2,3,4]: ";
    std::string columnsInput = readTextOrDefault("1,2,3,4");

    // Converte o texto das colunas em índices base 0
    std::vector<int> filters;
    for (std::string const & token : StringSplitter(columnsInput, ",").tokens()) {
        std::vector<int> range;
        if (StringSplitter::toIntegers(token, range)) {
            for (int entry : range) {
                if (entry > 0) filters.push_back(entry - 1);
            }
        }
    }

    // Cria a máscara de colunas
    if (!filters.empty()) {
        int maxFilter = *std::max_element(filters.begin(), filters.end());
        options.filterColumns.assign(maxFilter + 1, false);
        for (int f : filters) options.filterColumns.at(f) = true;
    }

    // Configuração padrão (igual à do parser de linha de comando)
    options.delimiter = ",";
    options.showStatistics = true;
    options.statisticsOptions.doMax = true;
    options.statisticsOptions.doMean = true;
    options.statisticsOptions.doMin = true;
    options.statisticsOptions.doVar = true;
    std::cout << "\n";
}

/**
 * @brief driver do programa
 * @param[in] argc Número de argumentos incluindo caminho do executável
 * @param[in] argv Array de caracteres dos argumentos
 * @return Status de saída
 *          - 0 = Sucesso
 *          - 1 = Falha no parsing de argumentos
 *          - 2 = Falha no cálculo de estatísticas
 *          - 3 = Falha na exibição de estatísticas
 */
int
pdrv(int argc,
     char * argv[])
{

    // ======================================================================
    // Analisa argumentos da linha de comando ou usa menu interativo
    // ----------------------------------------------------------------------
    CommandLineOptions options;

    if (argc <= 1) {
        // Sem argumentos: modo interativo
        interactiveMenu(options);
    } else {
        // Com argumentos: parser normal
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

    // ======================================================================
    // Lança aplicação e mede o tempo de processamento
    // ----------------------------------------------------------------------
    StatisticsApp application(options);

    try
    {
        auto t0 = std::chrono::steady_clock::now();
        int status = application.run();
        if (status != 0) return AppStatus::FAILED_RUN;
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();

        // Resumo no stderr (não polui o stdout comparável com diff)
        std::ostringstream info;
        info << std::fixed << std::setprecision(4);
        info << "[RESUMO] Modo: SEQUENCIAL | processos: 1 (PID " << getpid() << ")";
        info << " | tempo de processamento: " << secs << " s";
        std::cerr << info.str() << std::endl;
    }
    catch (std::exception & ex)
    {
        logMessage(Logger::Level::FATAL) << ex.what();
        return AppStatus::FAILED_RUN;
    }

    // ======================================================================
    // Exibe resultados
    // ----------------------------------------------------------------------
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

// Guarda de compilação: permite testar o código sem a função main
// quando _CLISTATS_TESTING está definido
#ifndef _CLISTATS_TESTING
int
main(int argc,
     char * argv[])
{
    return pdrv(argc, argv);
}
#endif