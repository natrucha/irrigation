/* 
Automated home irrigation based on weather data provided by CIMIS weather stations (https://cimis.water.ca.gov/Default.aspx).
Copyright (C) 2024  Natalie C. Pueyo Svoboda

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.


turn on drip/sprinkler systems based on calculations using
CIMIS provided ETo and UCANR SLIDE rules (https://ucanr.edu/sites/UrbanHort/Water_Use_of_Turfgrass_and_Landscape_Plant_Materials/SLIDE__Simplified_Irrigation_Demand_Estimation/)
to compute correct irrigation timing for the various garden locations. 
*/ 

#include <stdio.h>
#include <stdlib.h>

#include <jansson.h>     // json parser for C, see https://jansson.readthedocs.io/en/latest/ for documentation 
#include <curl/curl.h>   // see for examples https://curl.se/libcurl/c/example.html
#include <time.h>
#include <string.h>

// Include CMake input file, it's in the build folder so VSCode is freaking out
#include "IrrigationConfig.h"


#define BUFFER_SIZE (256 * 1024) /* 256 KB */


struct write_result {
    char *data;
    int pos;
};

static size_t write_response(void *ptr, size_t size, size_t nmemb, void *stream) {
    /* Save the GET response from into write_result struct, function from jannson exmaple code */
    struct write_result *result = (struct write_result *)stream;

    if (result->pos + size * nmemb >= BUFFER_SIZE - 1) {
        fprintf(stderr, "error: too small buffer\n");
        return 0;
    }

    memcpy(result->data + result->pos, ptr, size * nmemb);
    result->pos += size * nmemb;

    return size * nmemb;
}



static int newline_offset(const char *text) {
    /* Return the offset of the first newline in text or the length of
   text if there's no newline */
    const char *newline = strchr(text, '\n');
    if(!newline)
        return strlen(text);
    else
        return (int)(newline - text);
}



static char *request(const char *url) {
    /* CURL GET request from Jansson's github_commit.c example*/
    CURL *curl = NULL;
    CURLcode status;
    struct curl_slist *headers = NULL;
    char *data = NULL;
    long code;

    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    if (!curl)
        goto error;

    data = malloc(BUFFER_SIZE);
    if (!data)
        goto error;

    struct write_result write_result = {.data = data, .pos = 0};

    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* GitHub commits API v3 requires a User-Agent header */
    // headers = curl_slist_append(headers, "User-Agent: Jansson-Tutorial");
    // curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_result);

    status = curl_easy_perform(curl);
    if (status != 0) {
        fprintf(stderr, "error: unable to request data from %s:\n", url);
        fprintf(stderr, "%s\n", curl_easy_strerror(status));
        goto error;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 200) {
        fprintf(stderr, "error: server responded with code %ld\n", code);
        goto error;
    }

    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    curl_global_cleanup();

    /* zero-terminate the result */
    data[write_result.pos] = '\0';

    return data;

error:
    if (data)
        free(data);
    if (curl)
        curl_easy_cleanup(curl);
    if (headers)
        curl_slist_free_all(headers);
    curl_global_cleanup();
    return NULL;
}

int main(){
    char *cimis_station = CIMIS_STATION;
    char *cimis_app_key = APP_KEY;
    // printf("Example of using CMake input file, \n     Irrigation Major Version %d \n     Irrigation Minor Version %d \n", Irrigation_VERSION_MAJOR, Irrigation_VERSION_MINOR);

    const char *eto_value, *precip_value; // values adding up the total precipitation and ETo over specified range

    time_t date_today, date_start;
    char today_buffer[80], start_buffer[80];

    struct tm tm_out_today, tm_out_start;

    int num_days = 7; // how many days of data do we want to use?

    // obtain the Eto values for each day requested to CIMIS
    json_t *Data, *Providers, *get_records, *Records;

    float total_precipitation = 0.0; 
    float total_eto = 0.0;

    size_t i;
    char *text;

    json_t *root;
    json_error_t error;

    // provides the current date and time in seconds since the Epoch for the end date provided to CIMIS
    time(&date_today);
    // subtract the number of days (in seconds) of desired CIMIS data to obtain a start date
    date_start = date_today - num_days*(24*60*60);

    // represent the start and end dates in date and time components (local time)
    localtime_r(&date_today, &tm_out_today);
    localtime_r(&date_start, &tm_out_start);

    // format the strings according to the format specified by CIMIS 
    strftime(today_buffer, sizeof(today_buffer), "%Y-%m-%d", &tm_out_today);
    strftime(start_buffer, sizeof(start_buffer), "%Y-%m-%d", &tm_out_start);

    // stitch together the file name strings
    char *file_name = malloc(strlen("cimis_") + strlen(start_buffer) + strlen("_") + strlen(today_buffer) + strlen(".json") + 1); // +1 for the null-terminator
    // make function that has full_url passed as a pointer ref to concatenate the values internally, but can still free the pointer outside of the function
    // add checks for errors in malloc here
    strcpy(file_name, "cimis_");
    strcat(file_name, start_buffer);
    strcat(file_name, "_");
    strcat(file_name, today_buffer);
    strcat(file_name, ".json");
    printf("Testing to see if file %s exists\n", file_name);

    // check if call has already been made for this dataset by checking if a file already exists for it
    FILE *open_file = fopen(file_name, "r");
    if (open_file == NULL) {
        printf("File for desired dates does not exist, requesting data from CIMIS");

        // set up the url link with the API key, weather station number, and start and end dates for the data (obtain daily weather data)
        char *full_url = malloc(strlen("https://et.water.ca.gov/api/data?appKey=") + strlen(cimis_app_key) + strlen("&targets=") + strlen(cimis_station) + strlen("&startDate=") + strlen(start_buffer) + strlen("&endDate=") + strlen(today_buffer) + 1); 
        // make function that has full_url passed as a pointer ref to concatenate the values internally, but can still free the pointer outside of the function
        // add checks for errors in malloc here
        strcpy(full_url, "https://et.water.ca.gov/api/data?appKey=");
        strcat(full_url, cimis_app_key);
        strcat(full_url, "&targets=");
        strcat(full_url, cimis_station);
        strcat(full_url, "&startDate=");
        strcat(full_url, start_buffer);
        strcat(full_url, "&endDate=");
        strcat(full_url, today_buffer);

        // char* full_url = concat("https://et.water.ca.gov/api/data?appKey=a6de9bc2-642d-463d-9b14-967eb640a976&targets=171", "&startDate=", start_buffer, "&endDate=", today_buffer);
        printf("\nGET call to url: %s\n", full_url);

        // Jansson's function call to request data from url from Jannson's github_commit.c example
        text = request(full_url);
        if(!text)
            return 1;

        root = json_loads(text, 0, &error);
        // printf("%s", text);
        free(text);

        if (!root) {
            fprintf(stderr, "error: on line %d: %s\n", error.line, error.text);
            return 1;
        }

        // dump json into a file
        json_dump_file(root, file_name, 0);
        printf("JSON being saved to file: %s\n", file_name);

        free(full_url); // deallocate the file_name string
        printf("CIMIS data obtained and cached\n");

    }
    else {
        // The file exists which means a GET call has already been made for this date range
        printf("File for desired dates already exists, opening JSON file %s\n", file_name);
        root = json_load_file(file_name, 0, &error);
        if(!root) {
            printf("Could not open file %s\n", file_name);
            fprintf(stderr, "error: on line %d: %s\n", error.line, error.text);
            return 1;
        }
        // clean up by closing the file
        fclose(open_file);
        
    }
   
    

    Data = json_object_get(root, "Data");
    if (!json_is_object(Data)) {
        fprintf(stderr, "error: Data is not an object\n");
        // to check what type it actually is, go to https://jansson.readthedocs.io/en/2.8/apiref.html#c.json_type
        // and check typeof (it's an int and the types are listed in order)
        printf("    Data is a(n) %d\n", json_typeof(root));
        json_decref(root);
        return 1;
    } 
    // else {
    //     printf("Data correctly identified\n");
    // }
    
    Providers = json_object_get(Data, "Providers");
    if (!json_is_array(Providers)) {
        fprintf(stderr, "error: Providers is not an array\n");
        printf("    is Providers is an array? (1==yes) %d\n", json_is_array(Providers));
        printf("    otherwise, Providers is a(n) %d\n", json_typeof(Providers));
        json_decref(root);
        return 1; 
    } 
    // else {
    //     printf("Providers correctly identified\n");
    // }

    get_records = json_array_get(Providers, 0);
    if (!json_is_object(get_records)) {
        fprintf(stderr, "error: get_records is not an object\n");
        printf("    is get_records an array? (1==yes) %d\n", json_is_array(get_records));
        printf("    otherwise, get_records is a(n) %d\n", json_typeof(get_records));
        json_decref(root);
        return 1; 
    } 
    // else {
    //     printf("get_records correctly identified\n");
    // }

    Records = json_object_get(get_records, "Records");
    if (!json_is_array(Records)) {
        fprintf(stderr, "error: Records is not an array\n");
        printf("    Records is a(n) %d\n", json_typeof(Records));
        json_decref(root);
        return 1;
    }
    else {
        printf("The number of days being analyzed is %lu days\n", json_array_size(Records));
    }

    for (int i = 0; i < json_array_size(Records); i++) {
        json_t *get_daydata, *DayAsceEto, *EToValue, *DayPrecip, *PrecipValue;


        // Records is an array with an Eto value for each day requested in the url
        get_daydata = json_array_get(Records, i);
        if (!json_is_object(get_daydata)) {
            fprintf(stderr, "error: get_daydata is not an object\n");
            printf("    is get_daydata an array? (1==yes) %d\n", json_is_array(get_daydata));
            printf("    otherwise, get_daydata is a(n) %d\n", json_typeof(get_daydata));
            json_decref(root);
            return 1; 
        } 
        // else {
        //     printf("get_daydata correctly identified\n");
        // }

        // get the ETo data for the day
        DayAsceEto = json_object_get(get_daydata, "DayAsceEto");
        if (!json_is_object(DayAsceEto)) {
            fprintf(stderr, "error: DayAsceEto is not an object\n");
            printf("    DayAsceEto is a(n) %d\n", json_typeof(DayAsceEto));
            json_decref(root);
            return 1;
        } 
        // else {
        //     printf("DayAsceEto correctly identified\n");
        // }

        EToValue = json_object_get(DayAsceEto, "Value");
        if (!json_is_string(EToValue)) {
            fprintf(stderr, "error: EToValue is not a string\n");
            printf("    EToValue is a(n) %d\n", json_typeof(EToValue));
            json_decref(root);
            return 1;
        } 
        // else {
        //     printf("ETo value for the day is a string equal to:\n");
        // }

        eto_value = json_string_value(EToValue);
            // printf("%.8s %.*s\n", json_string_value(Providers), newline_offset(eto_value),
            //     eto_value);

        total_eto = total_eto + strtof(eto_value, NULL);

        // get the Precipitation data for the day
        DayPrecip = json_object_get(get_daydata, "DayPrecip");
        if (!json_is_object(DayPrecip)) {
            fprintf(stderr, "error: DayPrecip is not an object\n");
            printf("    DayPrecip is a(n) %d\n", json_typeof(DayPrecip));
            json_decref(root);
            return 1;
        } //else {
        //     printf("DayPrecip correctly identified\n");
        // }

        PrecipValue = json_object_get(DayPrecip, "Value");
        if (json_is_null(PrecipValue)) { 
            // precipitation could be null, just set as zero for now (likely that they don't have the data for the whole day so they provide null)
            // precip_value = json_string_value(PrecipValue);
            printf("Precipitation value for the day is null (probably the last day if only one message appears!)\n");
        } else if (!json_is_string(PrecipValue)) {
            fprintf(stderr, "error: PrecipValue is not a string\n");
            printf("    PrecipValue is a(n) %d\n", json_typeof(PrecipValue));
            json_decref(root);
            return 1;
        } else {
            // printf("DayPrecip value for the day is a string equal to:\n");
            precip_value = json_string_value(PrecipValue);
            // printf("%.8s %.*s\n", json_string_value(Providers), newline_offset(precip_value),
            // precip_value);

            total_precipitation = total_precipitation + strtof(precip_value, NULL);
        }
   
    }
    
    printf("\n\nThe total eto is             %f\n", total_eto);
    printf("The total precipitation is   %f (inches of rain)\n\n", total_precipitation);

    float veggie, backyard, inside, berry, front, grape;

    float effective_precipitation = total_precipitation * 0.5;


    // calculations for each garden location:
    //       Veggie patch:     PF = 1.0, LA = 96 sq. ft
    veggie = (total_eto * 1.0 * 96.0) - effective_precipitation;
    if (veggie <= 0.0) { 
        veggie = 0.0; 
        printf("veggie should be watered!\n");
    } else {
        printf("veggie remaining water %f (inches of water)\n", veggie);
    }

    //       Backyard:         PF = 0.5, LA = 300? sq. ft
    backyard = (total_eto * 0.5 * 300.0) - effective_precipitation;
    if (backyard <= 0.0) { 
        backyard = 0.0; 
        printf("backyard should be watered!\n");
    } else {
        printf("backyard remaining water %f (inches of water)\n", backyard);
    }

    //       Inside Side Yard: PF = 0.5?, LA = 64? sq. ft
    inside = (total_eto * 0.5 * 64.0) - effective_precipitation;
    if (inside <= 0.0) { 
        inside = 0.0; 
        printf("inside should be watered!\n");
    } else {
        printf("inside remaining water %f (inches of water)\n", inside);
    }

    //       Berry Patch:      PF = 0.8?, LA = 64? sq. ft
    berry = (total_eto * 0.8 * 64.0) - effective_precipitation;
    if (berry <= 0.0) { 
        berry = 0.0;
        printf("berry should be watered!\n");
    } else {
        printf("berry remaining water %f (inches of water)\n", berry);
    }

    //       Native Front:     PF = 0.3?, LA = 32? sq. ft
    front = (total_eto * 0.3 * 32.0) - effective_precipitation;
    if (front <= 0.0) { 
        front = 0.0; 
        printf("front should be watered!\n");
    } else {
        printf("front remaining water %f (inches of water)\n", front);
    }

    //       Grape:            PF = 0.5?, LA = 8 sq. ft
    grape = (total_eto * 0.5 * 8.0) - effective_precipitation;
    if (grape <= 0.0) { 
        grape = 0.0; 
        printf("grape should be watered!\n");
    }  else {
        printf("grape remaining water %f (inches of water)\n", grape);
    }

    json_decref(root);
    free(file_name); // deallocate the file_name string
    
    return(0);
}
