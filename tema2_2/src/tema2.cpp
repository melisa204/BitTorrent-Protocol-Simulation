#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <cstring>
#include <fstream>
#include <vector>
#include <mutex>
#include <algorithm>

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32 
#define MAX_CHUNKS 100
#define MAX_CLIENTS 10

// declaratie de structura
struct Swarm_info {
    vector<string> segments;
    vector<int> owners;
};

struct File_info {
    string filename;
    Swarm_info swarm;
};

struct Download_info {
    int rank;
    vector<string> wanted_files;
};

struct Owned_file {
    string filename;
    vector<string> segments;
};

// declaratii globale
vector<Owned_file> owned_files;
mutex owned_files_mutex;

// declaratii de prototipuri
void tracker(int numtasks, int rank);
vector<File_info> receive_data_from_peers(int numtasks);
void send_ack_message(int numtasks);
void send_done_message(int numtasks);
void deal_with_clients(int numtasks, vector<File_info> files);
bool send_swarm_to_client(vector<File_info> files, char *buffer, int source);
vector<string> send_data_to_tracker(int rank);
void peer(int numtasks, int rank);
void *upload_thread_func(void *arg);
void write_in_file(Download_info download_info, int i, vector<string> sorted_segments);
void download_segment(Swarm_info &swarm_info, Download_info download_info, vector<string> &owned_segments, int *downloaded_files, int i);
void get_segments(Swarm_info &swarm_info, Download_info download_info, vector<string> &owned_segments, int *downloaded_files, int i, int j);
void send_swarm_info(Swarm_info &swarm, int dest);
Swarm_info receive_swarm_info(int source);
vector<string> sort_segments(const vector<string> &segments, const Swarm_info &swarm_info);
void *download_thread_func(void *arg);

int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}

void tracker(int numtasks, int rank) {
    // primesc datele de la peers ca sa initializez structura
    vector<File_info> files = receive_data_from_peers(numtasks);

    // dau mesaj de ACK ca pot sa inceapa sa ceara si sa imparta chestii procesele
    send_ack_message(numtasks);

    // astept cereri de la clienti
    deal_with_clients(numtasks, files);

    // acum ca au terminat toti clientii -> trimit mesaj de terminare
    send_done_message(numtasks);

    return;
}

vector<File_info> receive_data_from_peers(int numtasks) {
    // declar structura in care salvez datele
    vector<File_info> files;

    // parametrul dupa care ma ghidez ca sa stiu de la cate procese am primit datele
    int received_data = 0;

    // il folosesc ca sa aflu de la care proces am primit datele
    MPI_Status status;

    // datele pe care trebuie sa le primesc si sa le pun in structura
    int number_of_files, number_of_segments;
    char filename[MAX_FILENAME + 1], segment[HASH_SIZE + 1];

    bool found;

    while (received_data < numtasks - 1) {
        // primesc numarul de fisiere de la un proces
        MPI_Recv(&number_of_files, 1, MPI_INT, MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);

        // salvez rankul ca acum sa primesc doar de la el
        int rank_status = status.MPI_SOURCE;

        for (int i = 0; i < number_of_files; i++) {
            // primesc numele fisierului
            MPI_Recv(filename, MAX_FILENAME + 1, MPI_CHAR, rank_status, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // primesc numarul de segmente
            MPI_Recv(&number_of_segments, 1, MPI_INT, rank_status, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // adaug datele in structura
            found = false; // ca sa verific daca fisierul exista deja in structura
            int file_index = 0;

            for (int j = 0; j < files.size(); j++) {
                if (files[j].filename == filename) {
                    found = true;
                    file_index = j; // salvez indexul fisierului din structura
                    break;
                }
            }

            if (found) {
                // inseamna ca deja fisierul e in structura cu toate segmentele lui, deci mai am de adaugat doar ownerul
                files[file_index].swarm.owners.push_back(rank_status);

                // doar primesc segmentele, nu le si adaug in structra, deoarece sunt deja acolo
                for (int j = 0; j < number_of_segments; j++) {
                    // primesc segmentul
                    MPI_Recv(segment, HASH_SIZE + 1, MPI_CHAR, rank_status, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                }

                continue;
            }
            
            // daca am ajuns aici inseamna ca e prima data cand apare fisierul asta -> il pun in structura
            File_info new_file;

            // pun numele fisierului
            new_file.filename = filename;

            // pun ownerul
            new_file.swarm.owners.push_back(rank_status);

            for (int j = 0; j < number_of_segments; j++) {
                // primesc segmentul
                MPI_Recv(segment, HASH_SIZE + 1, MPI_CHAR, rank_status, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

                // il adaug in structura
                new_file.swarm.segments.push_back(string(segment));
            }

            // adaug fisierul in structura
            files.push_back(new_file);
        }

        // incrementez datele primite
        received_data++;
    }

    return files;
}

void send_ack_message(int numtasks) {
    char message_ack[4];
    strcpy(message_ack, "ACK");

    for (int i = 1; i < numtasks; i++) {
        MPI_Send(message_ack, 4, MPI_CHAR, i, 0, MPI_COMM_WORLD);
    }
}

void send_done_message(int numtasks) {
    char message[5];
    strcpy(message, "DONE");

    for (int i = 1; i < numtasks; i++) {
        MPI_Send(message, 5, MPI_CHAR, i, 11, MPI_COMM_WORLD);
    }
}

void deal_with_clients(int numtasks, vector<File_info> files) {
    // mesajul de la clienti
    char buffer[MAX_FILENAME + 1]; // pentru terminator

    // numarul de clienti care si au terminat treaba
    int finished_clients = 0;

    while (finished_clients < numtasks - 1) {
        MPI_Status status;
        MPI_Recv(buffer, MAX_FILENAME + 1, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        // decid ce tip ce cerere am primit
        int tag = status.MPI_TAG;
        int source = status.MPI_SOURCE;
        bool found = false;

        switch(tag) {
            case 0:
                // cerere de download -> trimit swarmul fisierului
                found = send_swarm_to_client(files, buffer, source);

                if (!found) {
                    // presupunand ca se cere ceva ce nu exista
                }

                break;
            case 1:
                // inseamna ca a terminat de descarcat un fisier -> marchez simbolic cazul asta
                // deoarece am doar lista de owneri pe care am actualizat o deja
                break;
            case 2: 
                // inseamna ca un client si a terminat toata treaba -> incrementez numarul de clienti care au terminat
                finished_clients++;       
                break;
            default:
                break;
        }
    }
}

bool send_swarm_to_client(vector<File_info> files, char *buffer, int source) {
    for (int i = 0; i < files.size(); i++) {
        if (files[i].filename == buffer) {
            // adaug in swarm ownerul care a cerut fisierul
            files[i].swarm.owners.push_back(source);

            // trimit swarmul fisierului
            send_swarm_info(files[i].swarm, source);

            // am gasit fisierul
            return true;
        }
    }

    return false;
}

vector<string> send_data_to_tracker(int rank) {
    // formez numele fisierului
    char filename[MAX_FILENAME + 1];
    snprintf(filename, sizeof(filename), "in%d.txt", rank);

    // deschid fisierul
    ifstream file(filename);
    if (!file) {
        printf("Eroare la deschiderea fisierului %s\n", filename);
        exit(-1);
    }

    // citesc datele din fisier
    int num_of_files, num_of_segments;
    char owned_file[MAX_FILENAME + 1], segment[HASH_SIZE + 1];

    file >> num_of_files;
    // trimit numarul de fisiere catre tracker
    MPI_Send(&num_of_files, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

    for (int i = 0; i < num_of_files; i++) {
        // citesc fisierul pe care il detine
        file >> owned_file;

        Owned_file current_file;
        current_file.filename = owned_file;

        // trimit fisierul catre tracker
        MPI_Send(&owned_file, MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

        file >> num_of_segments;

        // trimit numarul de segmente catre tracker
        MPI_Send(&num_of_segments, 1, MPI_INT, TRACKER_RANK, 0, MPI_COMM_WORLD);

        for (int j = 0; j < num_of_segments; j++) {
            file >> segment;

            current_file.segments.push_back(segment);

            // trimit segmentul catre tracker
            MPI_Send(&segment, HASH_SIZE + 1, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        }

        owned_files.push_back(current_file);
    }

    int num_wanted_files;
    char wanted_file[MAX_FILENAME + 1];
    vector<string> wanted_files;

    // citesc numarul de fisiere pe care le vreau
    file >> num_wanted_files;

    // citesc fisierele pe care le vreau
    for (int i = 0; i < num_wanted_files; i++) {
        file >> wanted_file;
        wanted_files.push_back(wanted_file);
    }

    // inchid fisierul
    file.close();

    // returnez fisierele pe care le vreau
    return wanted_files;
}

void send_swarm_info(Swarm_info &swarm, int dest) {
    // trimit numarul de segmente 
    int num_segments = swarm.segments.size();
    MPI_Send(&num_segments, 1, MPI_INT, dest, 7, MPI_COMM_WORLD);

    // trimit fiecare segment
    for (size_t i = 0; i < swarm.segments.size(); i++) {
        MPI_Send(swarm.segments[i].c_str(), HASH_SIZE + 1, MPI_CHAR, dest, 8, MPI_COMM_WORLD);
    }

    // trimit numarul de owneri
    int num_owners = swarm.owners.size();
    MPI_Send(&num_owners, 1, MPI_INT, dest, 9, MPI_COMM_WORLD);

    // trimit ownerii
    MPI_Send(swarm.owners.data(), num_owners, MPI_INT, dest, 10, MPI_COMM_WORLD);
}

Swarm_info receive_swarm_info(int source) {
    Swarm_info swarm;

    // primesc nr de segmente
    int num_segments;
    MPI_Recv(&num_segments, 1, MPI_INT, source, 7, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // primeste fiecare segment
    for (int i = 0; i < num_segments; i++) {
        char segment[HASH_SIZE + 1];
        MPI_Recv(segment, HASH_SIZE + 1, MPI_CHAR, source, 8, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        swarm.segments.push_back(string(segment));
    }

    // primesc nr de owneri
    int num_owners;
    MPI_Recv(&num_owners, 1, MPI_INT, source, 9, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    swarm.owners.resize(num_owners);

    // primesc fiecare owner
    MPI_Recv(swarm.owners.data(), num_owners, MPI_INT, source, 10, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // returnez swarmul
    return swarm;
}

void peer(int numtasks, int rank) {
    // mai intai initializez datele -> le trimit la tracker
    vector<string> wanted_files;
    wanted_files = send_data_to_tracker(rank);

    char message[4];
    // acum astept ca toate procesele sa trimita datele la tracker
    MPI_Recv(message, 4, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    // salvez datele in structura pe care o trimit la download
    Download_info download_info;
    download_info.rank = rank;
    download_info.wanted_files = wanted_files;

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &download_info);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &rank);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}

void *upload_thread_func(void *arg) {
    int rank = *(int*) arg;

        while (true) {
            char segment[HASH_SIZE + 1];
            MPI_Status status;

            // astept cerere de la alt client
            MPI_Recv(segment, HASH_SIZE + 1, MPI_CHAR, MPI_ANY_SOURCE, 11, MPI_COMM_WORLD, &status);

            // aflu de la ce client e cererea
            int source = status.MPI_SOURCE;

            // verific daca e mesajul de final 
            if (strcmp(segment, "DONE") == 0 && source == TRACKER_RANK) {
                // toti clientii si au terminat executia -> ies din bucla
                break;
            }

            // verific daca am segmentul
            bool found = false;
            
            // pun mutex
            owned_files_mutex.lock();

            // caut i fisierele pe care le detin
            for (int i = 0; i < owned_files.size(); i++) {
                if (find(owned_files[i].segments.begin(), owned_files[i].segments.end(), string(segment)) != owned_files[i].segments.end()) {
                    found = true;
                    break;
                }
            }

            // eliberez mutexul
            owned_files_mutex.unlock();

            char response[5];

            // trimit raspuns in functie de caz
            if (found) {
                // inseamna ca il detin 
                strcpy(response, "ACK");
                MPI_Send(response, 5, MPI_CHAR, source, 12, MPI_COMM_WORLD);
            } else {
                // nu l am gasit 
                strcpy(response, "NACK");
                MPI_Send(response, 5, MPI_CHAR, source, 12, MPI_COMM_WORLD);
            }
        }

    return NULL;
}

void write_in_file(Download_info download_info, int i, vector<string> sorted_segments) {
    // creez numele fisierului
    string output_filename = "client" + to_string(download_info.rank) + "_" + download_info.wanted_files[i];

    // deschid fisierul
    ofstream output_file(output_filename);

    if (!output_file.is_open()) {
        cerr << "Eroare: Nu pot crea fișierul de ieșire " << output_filename << endl;
    }

    // scriu segmentele in fisier
    for (int j = 0; j < sorted_segments.size(); j++) {
        output_file << sorted_segments[j] << endl;
    }

    output_file.close();
}

void get_segments(Swarm_info &swarm_info, Download_info download_info, vector<string> &owned_segments, int *downloaded_files, int i, int j) {
    bool found =  false;
    int random_index, random_client;
    vector<int> unchecked_clients = swarm_info.owners;

    while (!found && !unchecked_clients.empty()) {
        random_index = rand() % unchecked_clients.size();
        random_client = unchecked_clients[random_index];

        // trimit cerere catre random_client
        MPI_Send(swarm_info.segments[j].c_str(), HASH_SIZE + 1, MPI_CHAR, random_client, 11, MPI_COMM_WORLD);

        char response[5];
        MPI_Recv(response, 5, MPI_CHAR, random_client, 12, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (strcmp(response, "ACK") == 0) {
            // am primit segmentul
            found = true;
            owned_segments.push_back(swarm_info.segments[j]);

            // adaug in variabila globala ca sa poata sa fie gasit de ceilalti clienti
            owned_files_mutex.lock();
            for (int k = 0; k < owned_files.size(); k++) {
                if (owned_files[k].filename == download_info.wanted_files[i]) {
                    owned_files[k].segments.push_back(swarm_info.segments[j]);
                    break;
                }
            }
            owned_files_mutex.unlock();

            // incrementez numarul de segmenete pe care le am descarcat 
            *downloaded_files = *downloaded_files + 1;
        }

        // elimin clientul din lista de clienti
        unchecked_clients.erase(unchecked_clients.begin() + random_index);
    }
}

void download_segment(Swarm_info &swarm_info, Download_info download_info, vector<string> &owned_segments, int *downloaded_files, int i) {
    for (int j = 0; j < swarm_info.segments.size(); j++) {
        // verific daca am segmentul
        bool found = false;

        for (int k = 0; k < owned_segments.size(); k++) {
            if (owned_segments[k] == swarm_info.segments[j]) {
                found = true;
                break;
            }
        }

        if (found) {
            // daca am segmentul, trec la urmatorul
            continue;
        }

        // daca nu am segmentul -> trimit cerere catre owneri pana gasesc segmentul
        get_segments(swarm_info, download_info, owned_segments, downloaded_files, i, j);

        // verific daca am descarcat 10 segmente
        if (*downloaded_files == 10) {
            MPI_Send(download_info.wanted_files[i].c_str(), MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD); // TOT CU TAGUL 0, DEOARECE ASTEAPTA SWARMUL INAPOI
            swarm_info = receive_swarm_info(TRACKER_RANK);
            *downloaded_files = 0;
        }

        // daca am ajuns aici si nu am gasit -> am pus de la mine verificare suplimentara
        if (!found) {
            // nu cred ca intru pe cazul asta
        }
    }
}

vector<string> sort_segments(const vector<string> &segments, const Swarm_info &swarm_info) {
    vector<int> sorted_indices(segments.size(), -1);
    vector<string> sorted_segments(segments.size());

    // calculez indicii pentru fiecare segment
    for (int i = 0; i < segments.size(); i++) {
        for (int j = 0; j < swarm_info.segments.size(); j++) {
            if (segments[i] == swarm_info.segments[j]) {
                sorted_indices[i] = j;
                break;
            }
        }
    }

    // sortez simultan indicii si segmentele
    for (int i = 0; i < sorted_indices.size() - 1; i++) {
        for (int j = i + 1; j < sorted_indices.size(); j++) {
            if (sorted_indices[i] > sorted_indices[j]) {
                // schimb indicii
                int aux_index = sorted_indices[i];
                sorted_indices[i] = sorted_indices[j];
                sorted_indices[j] = aux_index;

                // schimb segmentele
                string aux_segment = segments[i];
                sorted_segments[i] = segments[j];
                sorted_segments[j] = aux_segment;
            }
        }
    }

    // construiec vectorul sortat
    for (int i = 0; i < sorted_indices.size(); i++) {
        sorted_segments[i] = swarm_info.segments[sorted_indices[i]];
    }

    return sorted_segments;
}

void *download_thread_func(void *arg)
{
    Download_info download_info = *(Download_info*) arg;
    char message[11];

    int downloaded_files = 0;

    // iterez prin lista de fisiere pe care le vreau
    for (int i = 0; i < download_info.wanted_files.size(); i++) {
        // trimit cerere catre tracker pentru fisierul curent -> o sa primesc lista cu segmentele si ownerii

        MPI_Send(download_info.wanted_files[i].c_str(), MAX_FILENAME + 1, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        Swarm_info swarm_info = receive_swarm_info(TRACKER_RANK);

        // declar un vector de segmente pe care le am
        vector<string> owned_segments;

        // verific daca am deja fisierul -> ca sa respect principile protocolului
        for (int j = 0; j < owned_files.size(); j++) {
            if (owned_files[j].filename == download_info.wanted_files[i]) {
                owned_segments = owned_files[j].segments;
                break;
            }
        }

        // iterez prin lista de segmente si incerc sa le descarc
        download_segment(swarm_info, download_info, owned_segments, &downloaded_files, i);

        // sortez segmentele inainte de a le scrie in fisier
        vector<string> sorted_segments = sort_segments(owned_segments, swarm_info);

        // scriu in fisier
        write_in_file(download_info, i, sorted_segments);

        // anunt trackerul ca am terminat de descarcat un fisier
        strcpy(message, "DOWNLOADED");
        MPI_Send(message, 11, MPI_CHAR, TRACKER_RANK, 1, MPI_COMM_WORLD);
    }

    // aici am terminat de descarcat toate fisierele -> vad cum vb cu trackerul
    strcpy(message, "DONE");
    MPI_Send(message, 11, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD);

    return NULL;
}